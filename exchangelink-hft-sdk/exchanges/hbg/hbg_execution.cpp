#include "hbg_execution.h"
using namespace infra::hbg;

namespace infra {
bool HbgExecution::initialize() {
    auto& info = g_config_map[base_config_.to_str()];
    if (info.empty()) {
        INFRA_LOG_WARN("[hbg] [initialize] [fail], msg: {} {} {} not implemented", to_string(base_config_.account_type),
                       to_string(base_config_.address_type), to_string(base_config_.settle_unit));
        return false;
    }

    if (account_secret_.api_key.empty() || account_secret_.api_secret.empty()) {
        INFRA_LOG_WARN("[hbg] [initialize] [fail], msg: AccountSecret filed is empty");
        return false;
    }

    rest_host_ = info[REST_HOST];
    order_path_ = info[ORDER_PATH_PATH];
    query_order_path_ = info[QUERY_ORDER_PATH_PATH];
    cancel_order_path_ = info[CANCEL_ORDER_PATH_PATH];
    g_base_host = info["host_for_sign"]; // 兼容colo场景，签名使用的host单独配置

    wss_config_ = {info[WSS_PRIVATE_HOST], info[WSS_PORT], info[WSS_PRIVATE_PATH]};
    wss_trade_config_ = {info[WSS_TRADE_HOST], info[WSS_PORT], info[WSS_TRADE_PATH]};

    wss_stream_.set_user_data(0);
    wss_stream_.resolve_connect(wss_config_.host, wss_config_.port, wss_config_.path);

    wss_trade_.set_user_data(1);
    wss_trade_.resolve_connect(wss_trade_config_.host, wss_trade_config_.port, wss_trade_config_.path);

    INFRA_LOG_INFO("[hbg] [initialize] [Execution], websocket endpoint: {} {} {}", wss_config_.host, wss_config_.path,
                   wss_config_.port);
    INFRA_LOG_INFO("[hbg] [initialize] [Execution], websocket endpoint: {} {} {}", wss_trade_config_.host,
                   wss_trade_config_.path, wss_trade_config_.port);
    return true;
}

void HbgExecution::shutdown() {
    unsubscribe_order();
    if (wss_stream_.is_socket_open()) {
        wss_stream_.close();
    }
    if (wss_trade_.is_socket_open()) {
        wss_trade_.close();
    }
}

void HbgExecution::query_order(const SpOrder order, OrderCallback cb) {
    if (order->market_oid.empty()) {
        INFRA_LOG_WARN("[hbg] [query_order] [fail], msg: market_oid is empty");
        cb(Errno::InvalidParams, order);
        return;
    }
    std::string query = fmt::format("contract_code={}&order_id={}", order->pair, order->market_oid);
    auto req = get_request_body_with_sign(HTTP_GET, rest_host_, query_order_path_, query, "", account_secret_);
    send_http_request(req, order, cb, "query_order");
    INFRA_LOG_INFO("[hbg] [query_order], send: {}", query);
}

bool HbgExecution::subscribe_order(OrderCallback cb) {
    this->order_handler_ = std::move(cb);
    std::string payload =
        fmt::format(R"({{"op":"sub","cid":"{}","topic":"orders","contract_code":"*"}})", generate_req_id());
    return send_ws_request(wss_stream_, payload, "subscribe_order");
}

void HbgExecution::unsubscribe_order() {
    this->order_handler_ = nullptr;
    std::string payload =
        fmt::format(R"({{"op":"unsub","cid":"{}","topic":"orders","contract_code":"*"}})", generate_req_id());
    send_ws_request(wss_stream_, payload, "unsubscribe_order");
    INFRA_LOG_INFO("[hbg] [unsubscribe_order] [success]");
}

void HbgExecution::place_order(const SpOrder order, OrderCallback cb) {
    std::string payload{};

    if (order->type != OrderType::Limit && order->type != OrderType::Market) {
        INFRA_LOG_WARN("[hbg] [convert_place_order] [fail], msg: order type is not supported");
        cb(Errno::InvalidParams, order);
        return;
    }

    if (order->client_oid.empty() || order->pair.empty()) {
        INFRA_LOG_WARN("[hbg] [convert_place_order] [fail], msg: client_oid or pair is empty");
        cb(Errno::InvalidParams, order);
        return;
    }

    auto it = g_pairs_info_cache.find(to_lower_str(order->pair));
    if (it == g_pairs_info_cache.end()) {
        INFRA_LOG_WARN("[hbg] [convert_place_order] [fail], msg: not found {} in cache", order->pair);
        cb(Errno::InvalidParams, order);
        return;
    }

    SpExPairInfo pair_info = it->second;
    std::string margin_mode = "cross";
    std::string order_type;
    if (order->type == OrderType::Market) {
        order_type = "market";
    } else {
        order_type = "limit";
    }

    // hbg的post only表现在type字段
    if (order->tif == OrderTIF::MAKER) {
        order_type = "post_only";
    }

    std::string position_side;
    std::string side;
    if (g_current_position_mode == PositionMode::one_way_mode) {
        if (order->side == OrderSide::OpenLong || order->side == OrderSide::CloseShort) {
            side = "buy";
            position_side = "both";
        } else {
            side = "sell";
            position_side = "both";
        }
    } else {
        if (order->side == OrderSide::OpenLong) {
            side = "buy";
            position_side = "long";
        } else if (order->side == OrderSide::CloseLong) {
            side = "sell";
            position_side = "long";
        } else if (order->side == OrderSide::OpenShort) {
            side = "buy";
            position_side = "short";
        } else if (order->side == OrderSide::CloseShort) {
            side = "sell";
            position_side = "short";
        }
    }

    if (order->tif != OrderTIF::IOC && order->tif != OrderTIF::FOK && order->tif != OrderTIF::GTC && order->tif != OrderTIF::MAKER) {
        return;
    }

    std::string tif_str;
    switch (order->tif) {
        case OrderTIF::IOC:
            tif_str = "ioc";
            break;
        case OrderTIF::FOK:
            tif_str = "fok";
            break;
        default:
            tif_str = "gtc";
            break;
    }

    if (order->type == OrderType::Market) {
        tif_str = "gtc";
    }

    int volume = static_cast<int>(order->quantity / pair_info->denomination_value);             // 币数转张数
    double price = int(order->price / pair_info->step_size_quote) * pair_info->step_size_quote; // 调整价格精度

    std::string dynamic_parts;
    if (order->type == OrderType::Limit) {
        dynamic_parts += fmt::format(R"("price":"{}",)", price);
    }

    if (order->side == OrderSide::CloseLong || order->side == OrderSide::CloseShort) {
        dynamic_parts += R"("reduce_only":1)";
    } else {
        dynamic_parts += R"("reduce_only":0)";
    }

    payload = fmt::format(
        R"({{"contract_code":"{}","margin_mode":"{}","position_side":"{}",{},"side":"{}","type":"{}","time_in_force":"{}","client_order_id":"{}","volume":"{}"}})",
        transfer_from_infra_pair(order->pair), margin_mode, position_side, dynamic_parts, side, order_type, tif_str,
        order->client_oid, volume);

    order->uid = generate_req_id();
    std::string uid = "p" + std::to_string(order->uid);
    std::string api_order = fmt::format(R"({{"op":"place_order","cid":"{}","data":{}}})", uid, payload);
    send_ws_request(wss_trade_, api_order, "place_order_ws");
    // order->latency->sent_tsc = rdtsc();
    INFRA_LOG_INFO("[hbg] [place_order], send: {}", api_order);
    ws_request_cache_[uid] = std::make_pair(order, cb);
    // this->add_order_cache(order);
}

void HbgExecution::cancel_order(const SpOrder order, OrderCallback cb) {
    std::string payload = fmt::format(R"({{"contract_code":"{}","order_id":"{}"}})",
                                      transfer_from_infra_pair(order->pair), order->market_oid);
    std::string timestamp = std::to_string(time_get_now_sec());
    order->uid = generate_req_id();
    std::string uid = "c" + std::to_string(order->uid);
    std::string api_order = fmt::format(R"({{"op":"cancel_order","cid":"{}","data":{}}})", uid, payload);
    send_ws_request(wss_trade_, api_order, "cancel_order_ws");
    ws_request_cache_[uid] = std::make_pair(order, cb);
}

bool HbgExecution::send_ws_request(WebSocketClient& client, const std::string& content, const std::string& name) {
    if (name != "ping") {
        INFRA_LOG_INFO("[hbg] [{}], send: {}", name, content);
    }
    if (client.is_socket_open()) {
        client.send(content);
        return true;
    }
    INFRA_LOG_WARN("[hbg] [{}] [fail], msg: WebSocket not connected", name);
    return false;
}

Action HbgExecution::on_connect(Wss* ws) {
    INFRA_LOG_INFO("[hbg] [on_connect] [Execution], msg: WebSocket connection established");
    size_t index = ws->get_index();
    login(index);
    return Action::NONE;
}

Action HbgExecution::on_ping(Wss* ws, std::string_view payload) {
    // INFRA_LOG_DEBUG("[hbg] [on_ping] [Execution], payload: {}", payload);
    std::string content = fmt::format(R"({{"op":"pong","ts":"{}"}})", payload);
    size_t index = ws->get_index();
    if (index == 0) {
        send_ws_request(wss_stream_, content, "ping");
    } else if (index == 1) {
        send_ws_request(wss_trade_, content, "ping");
    }
    return Action::NONE;
}

Action HbgExecution::on_pong(Wss* ws, std::string_view payload) {
    // INFRA_LOG_DEBUG("[hbg] [on_pong] [Execution], payload: {}", payload);
    return Action::NONE;
}

void HbgExecution::on_close(Wss* ws) {
    INFRA_LOG_WARN("[hbg] [on_close] [Execution], msg: WebSocket connection has been closed, index: {}",
                   ws->get_index());
}

void HbgExecution::on_error(Wss* ws, std::string_view err) {
    INFRA_LOG_WARN("[hbg] [on_error] [Execution], msg: WebSocket error occurred: {}, index: {}", err,
                   ws->get_index());
}

Action HbgExecution::on_message(Wss* ws, std::string_view msg) {
    std::string decode_msg = hbg_decompress_gzip(msg);
    if (decode_msg.find("ping") == std::string::npos) {
        INFRA_LOG_INFO("[hbg] [on_message] [Execution], msg: {}", decode_msg);
    }
    try {
        PARSE_JSON(decode_msg, doc);
        if (doc["code"].error() == simdjson::SUCCESS) {
            int64_t rt_code = doc["code"];
            if (doc["cid"].error() == simdjson::SUCCESS) {
                std::string_view reqId = doc["cid"];
                std::string uid(reqId);
                std::string_view message = doc["message"];

                auto iter = ws_request_cache_.find(uid);
                if (iter == ws_request_cache_.end()) {
                    INFRA_LOG_WARN("[hbg] [on_message] [fail], uid not find decode_msg:{}", decode_msg);
                    return Action::RECEIVE;
                }

                auto [order, cb] = ws_request_cache_[uid];
                if (rt_code != 200 || message != "Success") {
                    INFRA_LOG_WARN("[hbg] [on_message] [fail], msg: {}", decode_msg);
                    order->ec = extract_error_code(decode_msg);
                    order->detail = message;
                    order->status = OrderStatus::Failed;
                    order->milli = time_get_now_milli();
                    cb(order->ec, order);
                    ws_request_cache_.erase(iter);
                    return Action::RECEIVE;
                }

                if (uid.size() > 1 && uid[0] == 'p') {
                    std::string orderId(doc["data"]["order_id"].get_string().value());
                    order->market_oid = orderId;
                    order->status = OrderStatus::New;
                    INFRA_LOG_INFO("[hbg] [place_order_ws] [success], recv: {}", decode_msg);
                } else {
                    order->status = OrderStatus::Canceling;
                    INFRA_LOG_INFO("[hbg] [cancel_order_ws] [success], recv: {}", decode_msg);
                }

                order->milli = time_get_now_milli();
                cb(Errno::Ok, order);
                ws_request_cache_.erase(iter);
            } else if (rt_code != 200) {
                INFRA_LOG_WARN("[hbg] [on_message] [fail], msg: {}", decode_msg);
            }
        } else if (doc["op"].error() == simdjson::SUCCESS) {
            std::string op(doc["op"].get_string().value());
            if (op == "ping") {
                on_ping(ws, doc["ts"]);
            } else if (op == "notify") {
                std::string topic(doc["topic"].get_string().value());
                if (topic == "orders") {
                    SpOrder rtn_order = parse_rtn_order(doc["data"], "order_update");
                    this->process_rtn_order(std::move(rtn_order));
                }
            } else if (op == "auth") {
                std::int64_t err_code(doc["err-code"].get_int64().value());
                if (err_code != 0) {
                    INFRA_LOG_WARN("[hbg] [on_message] [fail], login failed:{}", decode_msg);
                } else {
                    INFRA_LOG_INFO("[hbg] [on_message] [Execution], login success");
                }
            }
        }
    } catch (const std::exception& ex) {
        INFRA_LOG_WARN("[hbg] [on_message] [Execution] [exception], error: {}, msg: {}", ex.what(), decode_msg);
    }
    return Action::RECEIVE;
}

void HbgExecution::login(size_t index) {
    std::string time_str = time_get_now_str();
    std::string req_id = std::to_string(generate_req_id());

    if (index == 0) {
        std::string sign =
            get_websocket_sign(wss_config_.host, "/ws/v5/notification", "", "auth", time_str, account_secret_);
        std::string payload = fmt::format(
            R"({{"op":"auth","type":"api","AccessKeyId":"{}","SignatureMethod":"HmacSHA256","SignatureVersion":"2","Timestamp":"{}","Signature":"{}","cid":"{}"}})",
            account_secret_.api_key, time_str, sign, req_id);
        send_ws_request(wss_stream_, payload, "login");
    } else if (index == 1) {
        std::string sign =
            get_websocket_sign(wss_config_.host, "/linear-swap-trade", "", "auth", time_str, account_secret_);
        std::string payload = fmt::format(
            R"({{"op":"auth","type":"api","AccessKeyId":"{}","SignatureMethod":"HmacSHA256","SignatureVersion":"2","Timestamp":"{}","Signature":"{}","cid":"{}"}})",
            account_secret_.api_key, time_str, sign, req_id);
        send_ws_request(wss_trade_, payload, "login");
    }
}

void HbgExecution::send_http_request(const HttpRequestBody& req, SpOrder order, OrderCallback cb,
                                     std::string_view name) {
    client_.send(req, [this, order, cb, name](HttpResponseBody& res) {
        std::string response = boost::beast::buffers_to_string(res.body().data());
        do {
            if (res.result() != HTTP_STATUS_OK) {
                break;
            }
            try {
                PARSE_JSON(response, doc);
                if (doc["code"].error() == simdjson::SUCCESS && doc["code"].get_int64() == 1067 &&
                    name == "query_order") {
                    order->status = OrderStatus::Failed;
                    cb(Errno::OrderNotFound, order);
                    return;
                } else if (doc["code"].error() != simdjson::SUCCESS || doc["code"].get_int64() != 200) {
                    break;
                }
                if (name == "query_order") {
                    SpOrder rtn_order = parse_rtn_order(doc["data"], "query");
                    order->update(*rtn_order);
                }
                INFRA_LOG_INFO("[hbg] [{}] [success], recv: {}", name, response);
                order->milli = time_get_now_milli();
                cb(Errno::Ok, order);
                return;
            } catch (const std::exception& ex) {
                INFRA_LOG_WARN("[hbg] [{}], exception: {}", name, ex.what());
            }
        } while (0);
        INFRA_LOG_WARN("[hbg] [{}] [fail], recv: {}", name, response);
        order->ec = extract_error_code(response);
        order->detail = response;
        order->status = OrderStatus::Failed;
        order->milli = time_get_now_milli();
        cb(order->ec, order);
    });
}
} // namespace infra