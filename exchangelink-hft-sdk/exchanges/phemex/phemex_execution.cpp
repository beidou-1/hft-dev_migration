#include "phemex_execution.h"
using namespace infra::phemex;

namespace infra {
bool PhemexExecution::initialize() {
    auto& info = g_config_map[base_config_.to_str()];
    if (info.empty()) {
        INFRA_LOG_WARN("[phemex] [initialize] [fail], msg: {} {} {} not implemented",
                       to_string(base_config_.account_type), to_string(base_config_.address_type),
                       to_string(base_config_.settle_unit));
        return false;
    }

    if (account_secret_.api_key.empty() || account_secret_.api_secret.empty()) {
        INFRA_LOG_WARN("[phemex] [initialize] [fail], msg: AccountSecret filed is empty");
        return false;
    }

    rest_host_ = info[REST_HOST];
    query_order_path_ = info[QUERY_ORDER_PATH_PATH];
    place_order_path_ = info[PLACE_ORDER_PATH_PATH];
    cancel_order_path_ = info[CANCEL_ORDER_PATH_PATH];

    wss_config_ = {info[WSS_PRIVATE_HOST], info[WSS_PORT], info[WSS_PRIVATE_PATH]};
    wss_stream_.resolve_connect(wss_config_.host, wss_config_.port, wss_config_.path);
    INFRA_LOG_INFO("[phemex] [initialize] [Execution], websocket endpoint: {} {} {}", wss_config_.host,
                   wss_config_.path, wss_config_.port);
    return true;
}

void PhemexExecution::shutdown() { unsubscribe_order(); }

void PhemexExecution::query_order(const SpOrder order, OrderCallback cb) {
    if (order->market_oid.empty() || order->pair.empty()) {
        INFRA_LOG_WARN("[phemex] [query_order] [fail], msg: market_oid or pair is empty");
        cb(Errno::InvalidParams, order);
        return;
    }

    std::string query{};
    query.append("symbol=").append(transfer_from_infra_pair(order->pair));
    query.append("&orderID=").append(order->market_oid);
    auto req = get_request_body_with_sign(HTTP_GET, rest_host_, query_order_path_, query, account_secret_);
    send_http_request(req, order, cb, "query_order");
    INFRA_LOG_INFO("[phemex] [query_order], send: {}", query);
}

void PhemexExecution::place_order_rest(const SpOrder order, OrderCallback cb) {
    std::string payload{};
    if (!convert_place_order(order, cb, payload)) {
        return;
    }

    auto req = get_request_body_with_sign(HTTP_POST, rest_host_, place_order_path_, payload, account_secret_);
    send_http_request(req, order, cb, "place_order_rest");
    this->add_order_cache(order);
    INFRA_LOG_INFO("[phemex] [place_order_rest], send: {}", payload);
}

void PhemexExecution::cancel_order_rest(const SpOrder order, OrderCallback cb) {
    if (order->market_oid.empty() || order->pair.empty()) {
        INFRA_LOG_WARN("[phemex] [cancel_order_rest] [fail], msg: market_oid or pair is empty");
        cb(Errno::InvalidParams, order);
        return;
    }

    std::string payload{};
    std::string posSide = "Merged";
    if (g_current_position_mode == PositionMode::hedge_mode) {
        posSide = order->side == OrderSide::OpenLong || order->side == OrderSide::CloseLong ? "Long" : "Short";
    }
    payload.append("orderID=").append(order->market_oid);
    payload.append("&symbol=").append(transfer_from_infra_pair(order->pair));
    payload.append("&posSide=").append(posSide);
    auto req = get_request_body_with_sign(HTTP_DELETE, rest_host_, cancel_order_path_, payload, account_secret_);
    send_http_request(req, order, cb, "cancel_order");
    INFRA_LOG_INFO("[phemex] [cancel_order_rest], send: {}", payload);
}

bool PhemexExecution::subscribe_order(OrderCallback cb) {
    this->order_handler_ = std::move(cb);
    std::string payload = fmt::format(R"({{"method":"aop_p.subscribe","params":[],"id":{}}})", time_get_now_sec());
    return send_ws_request(wss_stream_, payload, "subscribe_order");
}

void PhemexExecution::unsubscribe_order() {
    this->order_handler_ = nullptr;
    std::string payload = fmt::format(R"({{"method":"aop_p.unsubscribe","params":[],"id":{}}})", time_get_now_sec());
    send_ws_request(wss_stream_, payload, "unsubscribe_order");
}

void PhemexExecution::place_order_ws(const SpOrder order, OrderCallback cb) { place_order_rest(order, cb); }

void PhemexExecution::cancel_order_ws(const SpOrder order, OrderCallback cb) { cancel_order_rest(order, cb); }

Action PhemexExecution::on_connect(Wss* ws) {
    INFRA_LOG_INFO("[phemex] [on_connect] [Execution], msg: WebSocket connection established");
    keep_ws_connection_alive(wss_stream_);
    login();
    return Action::NONE;
}

Action PhemexExecution::on_ping(Wss* ws, std::string_view payload) {
    // INFRA_LOG_DEBUG("[phemex] [on_ping] [Execution], payload: {}", payload);
    ws->pong(std::string(payload));
    return Action::NONE;
}

Action PhemexExecution::on_pong(Wss* ws, std::string_view payload) {
    // INFRA_LOG_DEBUG("[phemex] [on_pong] [Execution], payload: {}", payload);
    return Action::NONE;
}

void PhemexExecution::on_close(Wss* ws) {
    INFRA_LOG_WARN("[phemex] [on_close] [Execution], msg: WebSocket connection has been closed");
}

void PhemexExecution::on_error(Wss* ws, std::string_view err) {
    INFRA_LOG_WARN("[phemex] [on_error] [Execution], msg: WebSocket error occurred: {}", err);
}

Action PhemexExecution::on_message(Wss* ws, std::string_view msg) {
    // INFRA_LOG_INFO("[phemex] [on_message] [Execution], msg: {}", msg);
    try {
        PARSE_JSON(msg, doc);
        if (doc["orders_p"].error() == simdjson::SUCCESS) {
            simdjson::dom::array data = doc["orders_p"];
            for (auto item : data) {
                SpOrder rtn_order = parse_rtn_order(item);
                this->process_rtn_order(rtn_order);
            }
            std::string json_string = simdjson::minify(data);
            INFRA_LOG_INFO("[phemex] [on_message] [order], recv: {}", json_string);
        } else if (doc["position_info"].error() == simdjson::SUCCESS) {
            // 持仓变化不处理
        } else if (doc["index_market24h"].error() == simdjson::SUCCESS) {
            // ignore market24h msg
        } else if (doc["result"].error() == simdjson::SUCCESS) {
            // pong不处理
            if (doc["result"].is_object())
            {
                std::string_view status = doc["result"]["status"];
                if (status != "success") {
                    INFRA_LOG_WARN("[phemex] [on_message], unexcepted msg: {}", msg);
                }
            } else if (doc["result"].is_string()) {
                std::string_view ret = doc["result"];
                if (ret != "pong") {
                    INFRA_LOG_WARN("[phemex] [on_message], unexcepted msg: {}", msg);
                }
            }
        } else {
            INFRA_LOG_WARN("[phemex] [on_message], unexcepted msg: {}", msg);
        }
    } catch (const std::exception& ex) {
        INFRA_LOG_WARN("[phemex] [on_message] [exception], error: {}, msg: {}", ex.what(), msg);
    }
    return Action::RECEIVE;
}

void PhemexExecution::login() {
    std::string expire_second = std::to_string(time_get_now_sec() + 60);
    std::string sign = generate_sign_hmac256(account_secret_.api_secret, account_secret_.api_key + expire_second);
    std::string payload = fmt::format(R"({{"method":"user.auth","params":["API","{}","{}",{}],"id":12345}})",
                                      account_secret_.api_key, sign, expire_second);
    send_ws_request(wss_stream_, payload, "login");
}

bool PhemexExecution::convert_place_order(SpOrder order, OrderCallback cb, std::string& payload) {
    if (order->type != OrderType::Limit && order->type != OrderType::Market) {
        INFRA_LOG_WARN("[phemex] [convert_place_order] [fail], msg: order type is not supported");
        cb(Errno::InvalidParams, order);
        return false;
    }

    if (order->client_oid.empty() || order->pair.empty()) {
        INFRA_LOG_WARN("[phemex] [convert_place_order] [fail], msg: client_oid or pair is empty");
        cb(Errno::InvalidParams, order);
        return false;
    }

    auto it = g_pairs_info_cache.find(to_lower_str(order->pair));
    if (it == g_pairs_info_cache.end()) {
        INFRA_LOG_WARN("[phemex] [convert_place_order] [fail], msg: not found {} in cache", order->pair);
        cb(Errno::InvalidParams, order);
        return false;
    }

    SpExPairInfo pair_info = it->second;
    std::map<std::string, std::string> params;
    params["symbol"] = transfer_from_infra_pair(order->pair);
    std::string posSide{};
    bool reduceOnly = false;
    if (order->side == OrderSide::OpenLong) {
        params["side"] = "Buy";
        posSide = "Long";
    } else if (order->side == OrderSide::OpenShort) {
        params["side"] = "Sell";
        posSide = "Short";
    } else if (order->side == OrderSide::CloseLong) {
        params["side"] = "Sell";
        posSide = "Long";
        reduceOnly = true;
    } else if (order->side == OrderSide::CloseShort) {
        params["side"] = "Buy";
        posSide = "Short";
        reduceOnly = true;
    }
    params["posSide"] = g_current_position_mode == PositionMode::one_way_mode ? "Merged" : posSide;
    params["clOrdID"] = order->client_oid;
    switch (order->tif) {
        case OrderTIF::IOC:
            params["timeInForce"] = "ImmediateOrCancel";
            break;
        case OrderTIF::GTC:
            params["timeInForce"] = "GoodTillCancel";
            break;
        case OrderTIF::FOK:
            params["timeInForce"] = "FillOrKill";
            break;
        case OrderTIF::MAKER:
            params["timeInForce"] = "PostOnly";
            break;
        default:
            break;
    }
    bfloat price = order->price;
    switch (order->type) {
        case OrderType::Limit:
            params["ordType"] = "Limit";
            price = int(price / pair_info->step_size_quote) * pair_info->step_size_quote;
            params["priceRp"] = float_to_compact_str(price);
            break;
        case OrderType::Market:
            params["ordType"] = "Market";
            break;
        default:
            INFRA_LOG_WARN("[phemex] [convert_place_order] [fail], msg: order type is not supported");
            cb(Errno::InvalidParams, order);
            return false;
    }
    bfloat quantity = order->quantity;
    quantity = int(quantity / pair_info->step_size_base) * pair_info->step_size_base;
    params["orderQtyRq"] = float_to_compact_str(quantity);
    std::string request_str{};
    request_str.reserve(256);
    request_str.append("{");
    for (const auto& [key, value] : params) {
        request_str.append("\"" + key + "\"").append(":").append("\"" + value + "\",");
    }
    request_str.append(fmt::format(R"("reduceOnly":{}}})", reduceOnly));
    payload = request_str;
    return true;
}

void PhemexExecution::send_http_request(const HttpRequestBody& req, SpOrder order, OrderCallback cb,
                                        std::string_view name) {
    rest_.send(req, [this, order, cb, name](HttpResponseBody& res) {
        std::string response = boost::beast::buffers_to_string(res.body().data());
        do {
            if (res.result() != HTTP_STATUS_OK) {
                break;
            }
            try {
                PARSE_JSON(response, doc);
                if (doc["code"].error() != simdjson::SUCCESS || doc["code"].get_int64() != SUCCESS_CODE) {
                    break;
                }
                if (name == "place_order_rest") {
                    order->status = OrderStatus::New;
                } else if (name == "cancel_order_rest") {
                    order->status = OrderStatus::Canceling;
                } else if (name == "query_order") {
                    simdjson::dom::object data = doc["data"];
                    simdjson::dom::array orders = data["rows"];
                    if (orders.size() < 1) {
                        order->ec = Errno::OrderNotFound;
                        order->detail = response;
                        order->status = OrderStatus::Failed;
                        order->milli = time_get_now_milli();
                        cb(order->ec, order);
                    }
                    for (auto order_obj : orders) {
                        SpOrder updated_order = parse_rtn_order(order_obj, true);
                        order->update(*updated_order);
                        order->milli = time_get_now_milli();
                        cb(Errno::Ok, order);
                    }
                    return;
                }
                INFRA_LOG_INFO("[phemex] [{}] [success], recv: {}", name, response);
                order->milli = time_get_now_milli();
                cb(Errno::Ok, order);
                return;
            } catch (const std::exception& ex) {
                INFRA_LOG_WARN("[phemex] [{}], exception: {}", name, ex.what());
            }
        } while (0);
        INFRA_LOG_WARN("[phemex] [{}] [fail], recv: {}", name, response);
        order->ec = extract_error_code(response);
        order->detail = response;
        order->status = OrderStatus::Failed;
        order->milli = time_get_now_milli();
        cb(order->ec, order);
    });
}
} // namespace infra