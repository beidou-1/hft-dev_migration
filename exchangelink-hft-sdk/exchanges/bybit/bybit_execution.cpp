#include "bybit_execution.h"
using namespace infra::bybit;

namespace infra {
bool BybitExecution::initialize() {
    auto& info = g_config_map[base_config_.to_str()];
    if (info.empty()) {
        INFRA_LOG_WARN("[bybit] [initialize] [fail], msg: {} {} {} not implemented",
                       to_string(base_config_.account_type), to_string(base_config_.address_type),
                       to_string(base_config_.settle_unit));
        return false;
    }

    if (account_secret_.api_key.empty() || account_secret_.api_secret.empty()) {
        INFRA_LOG_WARN("[bybit] [initialize] [fail], msg: AccountSecret filed is empty");
        return false;
    }

    rest_host_ = info[REST_HOST];
    query_order_path_ = info[QUERY_ORDER_PATH_PATH];
    place_order_path_ = info[PLACE_ORDER_PATH_PATH];
    cancel_order_path_ = info[CANCEL_ORDER_PATH_PATH];
    category_ = "linear";

    wss_config_ = {info[WSS_PRIVATE_HOST], info[WSS_PORT], info[WSS_PRIVATE_PATH]};
    wss_stream_.set_user_data(0);
    wss_stream_.resolve_connect(wss_config_.host, wss_config_.port, wss_config_.path);
    INFRA_LOG_INFO("[bybit] [initialize] [Execution], websocket endpoint: {} {} {}", wss_config_.host, wss_config_.path,
                   wss_config_.port);

    wss_trade_config_ = {info[WSS_TRADE_HOST], info[WSS_PORT], info[WSS_TRADE_PATH]};
    wss_trade_.set_user_data(1);
    wss_trade_.resolve_connect(wss_trade_config_.host, wss_trade_config_.port, wss_trade_config_.path);
    INFRA_LOG_INFO("[bybit] [initialize] [Execution], websocket endpoint: {} {} {}", wss_trade_config_.host,
                   wss_trade_config_.path, wss_trade_config_.port);
    return true;
}

void BybitExecution::shutdown() {
    wss_stream_.close();
    wss_trade_.close();
}

void BybitExecution::query_order(const SpOrder order, OrderCallback cb) {
    if (order->market_oid.empty() || order->pair.empty()) {
        INFRA_LOG_WARN("[bybit] [query_order] [fail], msg: market_oid or pair is empty");
        cb(Errno::InvalidParams, order);
        return;
    }

    std::string query{};
    query.append("category=").append(category_);
    query.append("&orderId=").append(order->market_oid);
    query.append("&symbol=").append(transfer_from_infra_pair(order->pair));
    auto req = get_request_body_with_sign(HTTP_GET, rest_host_, query_order_path_, query, account_secret_);
    send_http_request(req, order, cb, "query_order");
    INFRA_LOG_INFO("[bybit] [query_order], send: {}", query);
}

bool BybitExecution::subscribe_order(OrderCallback cb) {
    this->order_handler_ = std::move(cb);
    std::string payload = R"({"op":"subscribe","args":["order"]})";
    return send_ws_request(wss_stream_, payload, "subscribe_order");
}

void BybitExecution::unsubscribe_order() {
    this->order_handler_ = nullptr;
    std::string payload = R"({"op":"unsubscribe","args":["order"]})";
    send_ws_request(wss_stream_, payload, "unsubscribe_order");
    INFRA_LOG_INFO("[bybit] [unsubscribe_order] [success]");
}

void BybitExecution::place_order(const SpOrder order, OrderCallback cb) {
    std::string order_params{};
    if (!convert_place_order(order, cb, order_params)) {
        return;
    }

    order->uid = generate_req_id();
    auto now_ms = std::to_string(time_get_now_milli());
    std::string payload = fmt::format(
        R"({{"reqId":"{}","header":{{"X-BAPI-TIMESTAMP":"{}","X-BAPI-RECV-WINDOW":"5000"}},"op":"order.create","args":[{}]}})",
        order->uid, now_ms, order_params);
    send_ws_request(wss_trade_, payload, "place_order_ws");
    ws_request_cache_[order->uid] = std::make_pair(order, cb);
}

void BybitExecution::cancel_order(const SpOrder order, OrderCallback cb) {
    std::string cancel_params{};
    if (!convert_cancel_order(order, cb, cancel_params)) {
        return;
    }

    order->uid = generate_req_id();
    auto now_ms = std::to_string(time_get_now_milli());
    std::string payload = fmt::format(
        R"({{"reqId":"{}","header":{{"X-BAPI-TIMESTAMP":"{}","X-BAPI-RECV-WINDOW":"5000"}},"op":"order.cancel","args":[{}]}})",
        order->uid, now_ms, cancel_params);
    send_ws_request(wss_trade_, payload, "cancel_order_ws");
    ws_request_cache_[order->uid] = std::make_pair(order, cb);
}

Action BybitExecution::on_connect(Wss* ws) {
    size_t index = ws->get_index();
    INFRA_LOG_INFO("[bybit] [on_connect] [Execution], msg: WebSocket connection established, index: {}", index);
    keep_ws_connection_alive(index);
    login(index);
    return Action::NONE;
}

Action BybitExecution::on_ping(Wss* ws, std::string_view payload) {
    // INFRA_LOG_DEBUG("[bybit] [on_ping] [Execution], payload: {}", payload);
    ws->pong(std::string(payload));
    return Action::NONE;
}

Action BybitExecution::on_pong(Wss* ws, std::string_view payload) {
    // INFRA_LOG_DEBUG("[bybit] [on_pong] [Execution], payload: {}", payload);
    return Action::NONE;
}

void BybitExecution::on_close(Wss* ws) {
    INFRA_LOG_WARN("[bybit] [on_close] [Execution], msg: WebSocket connection has been closed, connection ID: {}",
                   ws->get_index());
}

void BybitExecution::on_error(Wss* ws, std::string_view err) {
    INFRA_LOG_WARN("[bybit] [on_error] [Execution], msg: WebSocket error occurred: {}, index: {}", err,
                   ws->get_index());
}

Action BybitExecution::on_message(Wss* ws, std::string_view msg) {
    // INFRA_LOG_DEBUG("[bybit] [on_message] [Execution], msg: {}", msg);
    try {
        PARSE_JSON(msg, doc);
        if (doc["op"].error() == simdjson::SUCCESS) {
            std::string_view op = doc["op"];
            if (op == "order.create" || op == "order.cancel") {
                std::string_view reqId = doc["reqId"];
                unsigned long uid = std::stoul(std::string(reqId));
                auto iter = ws_request_cache_.find(uid);
                if (iter == ws_request_cache_.end()) {
                    INFRA_LOG_WARN("[bybit] [on_message] [fail], msg:", msg);
                    return Action::RECEIVE;
                }

                auto [order, cb] = ws_request_cache_[uid];
                if (doc["retCode"].get_int64() != BYBIT_SUCCESS_CODE) {
                    INFRA_LOG_WARN("[bybit] [on_message] [place_order_ws] [fail], msg: {}", msg);
                    order->ec = extract_error_msg(msg);
                    order->detail = msg;
                    order->status = OrderStatus::Failed;
                    order->milli = time_get_now_milli();
                    cb(order->ec, order);
                    ws_request_cache_.erase(iter);
                    return Action::RECEIVE;
                }
                if (op == "order.create") {
                    std::string_view orderId = doc["data"]["orderId"];
                    order->market_oid = orderId;
                    order->status = OrderStatus::New;
                    INFRA_LOG_INFO("[bybit] [place_order_ws] [success], msg: {}", msg);
                } else {
                    order->status = OrderStatus::Canceling;
                    INFRA_LOG_INFO("[bybit] [cancel_order_ws] [success], msg: {}", msg);
                }
                order->milli = time_get_now_milli();
                cb(Errno::Ok, order);
                ws_request_cache_.erase(iter);
            } else if (op == "auth" || op == "subscribe" || op == "unsubscribe") {
                bool success = false;
                if (doc["success"].error() == simdjson::SUCCESS) {
                    success = doc["success"];
                } else if (doc["retCode"].error() == simdjson::SUCCESS) {
                    success = (doc["retCode"].get_int64() == BYBIT_SUCCESS_CODE);
                }
                if (success) {
                    INFRA_LOG_INFO("[bybit] [{}] [success], msg: {}", op, msg);
                } else {
                    INFRA_LOG_WARN("[bybit] [{}] [fail], msg: {}", op, msg);
                }
            } else if (op == "ping" || op == "pong") {
                // ignore
            } else {
                INFRA_LOG_WARN("[bybit] [on_message], unexpected msg: {}", msg);
            }
        }
    } catch (const std::exception& ex) {
        INFRA_LOG_WARN("[bybit] [on_message], exception: {}, msg: {}", ex.what(), msg);
    }
    return Action::RECEIVE;
}

void BybitExecution::login(size_t index) {
    long long expires = time_get_now_milli() + 10000;
    if (index == 1) {
        expires += 10;
    }
    std::string val = "GET/realtime" + std::to_string(expires);
    std::string signature = generate_sign_hmac256(account_secret_.api_secret, val);
    std::string payload =
        fmt::format(R"({{"op":"auth","args":["{}",{},"{}"]}})", account_secret_.api_key, expires, signature);
    if (index == 0) {
        send_ws_request(wss_stream_, payload, "login");
    } else if (index == 1) {
        send_ws_request(wss_trade_, payload, "login");
    }
}

void BybitExecution::keep_ws_connection_alive(size_t index) {
    if (index == 0) {
        wss_stream_.start_ping_pong(R"({"op":"ping"})", 25);
    } else if (index == 1) {
        wss_trade_.start_ping_pong(R"({"op":"ping"})", 25);
    }
}

bool BybitExecution::send_ws_request(WebSocketClient& client, const std::string& content, const std::string& name) {
    if (client.is_socket_open()) {
        client.send(content);
        INFRA_LOG_INFO("[bybit] [{}], send: {}", name, content);
        return true;
    } else {
        INFRA_LOG_WARN("[bybit] [{}] [fail], msg: WebSocket not connected", name);
        return false;
    }
}

bool BybitExecution::convert_place_order(SpOrder order, OrderCallback cb, std::string& res) {
    if (order->type != OrderType::Limit && order->type != OrderType::Market) {
        INFRA_LOG_WARN("[bybit] [convert_place_order] [fail], msg: order type is not supported");
        cb(Errno::InvalidParams, order);
        return false;
    }

    if (order->client_oid.empty() || order->pair.empty()) {
        INFRA_LOG_WARN("[bybit] [convert_place_order] [fail], msg: client_oid or pair is empty");
        cb(Errno::InvalidParams, order);
        return false;
    }

    auto it = g_pairs_info_cache.find(to_lower_str(order->pair));
    if (it == g_pairs_info_cache.end()) {
        INFRA_LOG_WARN("[bybit] [convert_place_order] [fail], msg: not found {} in cache", order->pair);
        cb(Errno::InvalidParams, order);
        return false;
    }

    SpExPairInfo pair_info = it->second;
    double quantity = std::floor(order->quantity / pair_info->step_size_base) * pair_info->step_size_base; // 调整数量精度
    int qty_decimals = static_cast<int>(std::round(-std::log10(pair_info->step_size_base)));
    std::string qty_str = fmt::format("{:.{}f}", quantity, qty_decimals);

    double price{0};     // 调整价格精度
    switch (order->side) {
        case OrderSide::OpenLong:
            price = std::floor(order->price / pair_info->step_size_quote) * pair_info->step_size_quote;
            break;
        case OrderSide::OpenShort:
            price = std::ceil(order->price / pair_info->step_size_quote) * pair_info->step_size_quote;
            break;
        case OrderSide::CloseShort:
            price = std::floor(order->price / pair_info->step_size_quote) * pair_info->step_size_quote;
            break;
        case OrderSide::CloseLong:
            price = std::ceil(order->price / pair_info->step_size_quote) * pair_info->step_size_quote;
            break;
        default:
            break;
    }

    std::string side;
    int position_idx = 0;
    bool reduce_only = false;

    if (g_current_position_mode == PositionMode::one_way_mode) {
        position_idx = 0;
        if (order->side == OrderSide::OpenLong || order->side == OrderSide::CloseShort) {
            side = "Buy";
        } else {
            side = "Sell";
        }

        if (order->side == OrderSide::CloseLong || order->side == OrderSide::CloseShort) {
            reduce_only = true;
        }
    } else {
        if (order->side == OrderSide::OpenLong) {
            side = "Buy";
            position_idx = 1;
        } else if (order->side == OrderSide::CloseLong) {
            side = "Sell";
            position_idx = 1;
        } else if (order->side == OrderSide::OpenShort) {
            side = "Sell";
            position_idx = 2;
        } else if (order->side == OrderSide::CloseShort) {
            side = "Buy";
            position_idx = 2;
        }
    }

    std::string tif_str;
    switch (order->tif) {
        case OrderTIF::IOC:
            tif_str = "IOC";
            break;
        case OrderTIF::FOK:
            tif_str = "FOK";
            break;
        default:
            tif_str = "GTC";
            break;
    }

    std::string type_str = (order->type == OrderType::Market) ? "Market" : "Limit";
    int price_decimals = static_cast<int>(std::round(-std::log10(pair_info->step_size_quote)));
    std::string dynamic_parts;
    if (order->type != OrderType::Market) {
        dynamic_parts += fmt::format(R"("price":"{:.{}f}",)", price, price_decimals);
    }
    if (reduce_only) {
        dynamic_parts += R"("reduceOnly":true,)";
    }

    res = fmt::format(
        R"({{"category":"{}","symbol":"{}","side":"{}","orderType":"{}","qty":"{}",{}"marketUnit":"baseCoin","timeInForce":"{}","positionIdx":{},"orderLinkId":"{}"}})",
        category_, transfer_from_infra_pair(order->pair), side, type_str, qty_str,
        dynamic_parts,
        tif_str, position_idx, order->client_oid);
    return true;
}

bool BybitExecution::convert_cancel_order(SpOrder order, OrderCallback cb, std::string& res) {
    if (order->market_oid.empty() || order->pair.empty()) {
        INFRA_LOG_WARN("[bybit] [convert_cancel_order] [fail], msg: market_oid or pair is empty");
        cb(Errno::InvalidParams, order);
        return false;
    }

    res = fmt::format(R"({{"category":"{}","symbol":"{}","orderId":"{}"}})", category_,
                      transfer_from_infra_pair(order->pair), order->market_oid);
    return true;
}

void BybitExecution::send_http_request(const HttpRequestBody& req, SpOrder order, OrderCallback cb,
                                       std::string_view name) {
    rest_.send(req, [this, order, cb, name](HttpResponseBody& res) {
        std::string response = boost::beast::buffers_to_string(res.body().data());
        do {
            if (res.result() != HTTP_STATUS_OK) {
                break;
            }
            try {
                PARSE_JSON(response, doc);
                if (doc["retCode"].get_int64() != BYBIT_SUCCESS_CODE) {
                    break;
                }
                if (name == "place_order_rest") {
                    std::string_view orderId = doc["result"]["orderId"];
                    order->market_oid = orderId;
                    order->status = OrderStatus::New;
                } else if (name == "cancel_order_rest") {
                    order->status = OrderStatus::Canceling;
                } else if (name == "query_order") {
                    simdjson::dom::array list = doc["result"]["list"];
                    if (list.size() == 0) {
                        order->ec = Errno::OrderNotFound;
                        order->milli = time_get_now_milli();
                        cb(order->ec, order);
                        return;
                    }
                    for (auto item : list) {
                        SpOrder rtn_order = parse_rtn_order(item);
                        order->update(*rtn_order);
                        break; // 只取一个
                    }
                }
                INFRA_LOG_INFO("[bybit] [{}] [success], recv: {}", name, response);
                order->milli = time_get_now_milli();
                cb(Errno::Ok, order);
                return;
            } catch (const std::exception& ex) {
                INFRA_LOG_WARN("[bybit] [{}] [exception], msg: {}", name, ex.what());
            }
        } while (0);
        INFRA_LOG_WARN("[bybit] [{}] [fail], recv: {}", name, response);
        order->ec = extract_error_msg(response);
        order->detail = response;
        order->status = OrderStatus::Failed;
        order->milli = time_get_now_milli();
        cb(order->ec, order);
    });
}
} // namespace infra