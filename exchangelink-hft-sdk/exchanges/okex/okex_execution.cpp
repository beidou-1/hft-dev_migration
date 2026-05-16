#include "okex_execution.h"

using namespace infra::okex;
using namespace boost::beast;

namespace infra {
bool OkxExecution::initialize() {
    auto& info = g_config_map[base_config_.to_str()];
    if (info.empty()) {
        INFRA_LOG_WARN("[okex] [initialize] [fail], msg: {} {} {} not implemented",
                       to_string(base_config_.account_type), to_string(base_config_.address_type),
                       to_string(base_config_.settle_unit));
        return false;
    }

    rest_host_ = info[REST_HOST];
    query_order_path_ = info[QUERY_ORDER_PATH_PATH];
    place_order_path_ = info[PLACE_ORDER_PATH_PATH];
    cancel_order_path_ = info[CANCEL_ORDER_PATH_PATH];

    wss_config_ = {info[WSS_PRIVATE_HOST], info[WSS_PORT], info[WSS_PRIVATE_PATH]};
    wss_api_config_ = {info[WSS_PRIVATE_HOST], info[WSS_PORT], info[WSS_PRIVATE_PATH]};
    INFRA_LOG_INFO("[okex] [initialize] [Execution], websocket endpoint: {} {} {}", wss_config_.host, wss_config_.path,
                   wss_config_.port);

    wss_api_.set_user_data(0);
    wss_api_.resolve_connect(wss_api_config_.host, wss_api_config_.port, wss_api_config_.path);
    wss_stream_.set_user_data(1);
    wss_stream_.resolve_connect(wss_config_.host, wss_config_.port, wss_config_.path);
    return true;
}

void OkxExecution::shutdown() {
    unsubscribe_order();
    wss_api_.close();
}

void OkxExecution::query_order(const SpOrder order, OrderCallback cb) {
    std::string query{};
    if (order->pair.empty()) {
        INFRA_LOG_WARN("[okex] [query_order] [fail], msg: order pair is empty");
        cb(Errno::InvalidParams, order);
        return;
    }
    query.append("instId=").append(transfer_from_infra_pair(order->pair));
    if (!order->market_oid.empty()) {
        query.append("&ordId=").append(order->market_oid);
    } else if (!order->client_oid.empty()) {
        query.append("&clOrdId=").append(order->client_oid);
    } else {
        INFRA_LOG_WARN("[okex] [query_order] [fail], msg: both market_oid and client_oid are empty");
        cb(Errno::InvalidParams, order);
        return;
    }
    auto req = get_request_body_with_sign(http::verb::get, rest_host_, query_order_path_, query, account_secret_);
    send_http_request(req, order, cb, "query_order");
    INFRA_LOG_INFO("[okex] [query_order], send: {}", query);
}

bool OkxExecution::subscribe_order(OrderCallback cb) {
    this->order_handler_ = std::move(cb);
    std::string payload = R"({"op":"subscribe","args":[{"instType":"SWAP","channel":"orders"}]})";
    return send_ws_request(wss_stream_, payload, "subscribe_order");
}

void OkxExecution::unsubscribe_order() {
    this->order_handler_ = nullptr;
    std::string payload = R"({"op":"unsubscribe","args":[{"instType":"SWAP","channel":"orders"}]})";
    send_ws_request(wss_stream_, payload, "unsubscribe_order");
}

void OkxExecution::place_order(const SpOrder order, OrderCallback cb) {
    std::string payload{};
    if (!convert_place_order(order, cb, payload)) {
        return;
    }
    this->send_ws_request(wss_api_, std::move(payload), "place_order_ws");
    ws_request_cache_["pOrder" + order->client_oid] = std::make_pair(order, cb);
}

void OkxExecution::cancel_order(const SpOrder order, OrderCallback cb) {
    if (order->market_oid.empty() || order->pair.empty()) {
        INFRA_LOG_WARN("[okex] [cancel_order_ws] [fail], msg: market_oid or pair is empty");
        cb(Errno::InvalidParams, order);
        return;
    }

    auto it = g_pairs_info_cache.find(to_lower_str(order->pair));
    if (it == g_pairs_info_cache.end()) {
        INFRA_LOG_WARN("[okex] [cancel_order_ws] [fail], msg: not found {} in cache", order->pair);
        cb(Errno::InvalidParams, order);
        return;
    }

    SpExPairInfo pair_info = it->second;
    std::string instIdCode = pair_info->alias;
    std::string uid = "cOrder" + order->client_oid;
    std::string payload =
        fmt::format(R"({{"op":"cancel-order","id":"{}","args":[{{"ordId":"{}","instIdCode":"{}"}}]}})", uid,
                    order->market_oid, instIdCode);
    this->send_ws_request(wss_api_, std::move(payload), "cancel_order_ws");
    ws_request_cache_[uid] = std::make_pair(order, cb);
}

Action OkxExecution::on_connect(Wss* ws) {
    size_t index = ws->get_index();
    INFRA_LOG_INFO("[okex] [on_connect] [success], msg: WebSocket connection established, connection ID: {}", index);

    login(index);
    if (index == 0)
        keep_ws_connection_alive(wss_api_);
    else if (index == 1)
        keep_ws_connection_alive(wss_stream_);
    return Action::NONE;
}

Action OkxExecution::on_ping(Wss* ws, std::string_view payload) {
    // INFRA_LOG_DEBUG("[okex] [on_ping] [Execution], payload: {}", payload);
    ws->pong(std::string(payload));
    return Action::NONE;
}

Action OkxExecution::on_pong(Wss* ws, std::string_view payload) {
    // INFRA_LOG_DEBUG("[okex] [on_pong] [Execution], payload: {}", payload);
    return Action::NONE;
}

void OkxExecution::on_close(Wss* ws) {
    INFRA_LOG_WARN("[okex] [on_close] [fail], msg: WebSocket connection has been closed");
}

void OkxExecution::on_error(Wss* ws, std::string_view err) {
    INFRA_LOG_WARN("[okex] [on_error] [fail], msg: WebSocket error occurred: {}", err);
}

Action OkxExecution::on_message(Wss* ws, std::string_view msg) {
    // INFRA_LOG_INFO("[okex] [on_message] [success], msg: {}", msg);
    if (msg == "pong") {
        return Action::RECEIVE;
    }
    try {
        PARSE_JSON(msg, doc);
        if (doc["op"].error() == simdjson::SUCCESS) {
            std::string_view op = doc["op"];
            if (op == "order" || op == "cancel-order") {
                INFRA_LOG_INFO("[okex] [on_message], recv: {}", msg);
                std::string_view code = doc["code"];
                std::string_view msg = doc["msg"];
                simdjson::dom::array data = doc["data"];
                Errno ec = Errno::Ok;
                std::string_view id = doc["id"];
                std::string uid = std::string(id);
                auto iter = ws_request_cache_.find(uid);
                if (iter == ws_request_cache_.end()) {
                    // INFRA_LOG_WARN("[okex] [on_message] [fail], msg:", msg);
                    return Action::RECEIVE;
                }
                auto [order, cb] = ws_request_cache_[uid];
                if (msg.empty()) {
                    for (auto obj : data) {
                        order->market_oid = obj["ordId"];
                        if (code == "0") {
                            order->status = (op == "order") ? OrderStatus::New : OrderStatus::Canceling;
                        } else {
                            order->status = OrderStatus::Failed;
                            std::string_view error_msg = obj["sMsg"];
                            ec = extract_error_msg(error_msg);
                        }
                        cb(ec, order);
                    }
                } else {
                    order->status = OrderStatus::Failed;
                    order->detail = msg;
                    order->ec = extract_error_msg(msg);
                    order->milli = time_get_now_milli();
                    cb(order->ec, order);
                    ws_request_cache_.erase(iter);
                    return Action::RECEIVE;
                }
                ws_request_cache_.erase(iter);
            }
        } else if (doc["event"].error() == simdjson::SUCCESS) {
            std::string_view event = doc["event"];
            if (event == "login" || event == "subscribe") {
                INFRA_LOG_INFO("[okex] [on_message], recv: {}", msg);
            } else {
                INFRA_LOG_WARN("[okex] [on_message], unexcepted msg: {}", msg);
            }
        } else {
            INFRA_LOG_WARN("[okex] [on_message], unexpected msg: {}", msg);
        }
    } catch (const std::exception& ex) {
        INFRA_LOG_WARN("[okex] [on_message], exception: {}, msg: {}", ex.what(), msg);
    }
    return Action::RECEIVE;
}

void OkxExecution::login(int index) {
    std::string raw_str{};
    std::string timestamp = std::to_string(time_get_now_sec());
    raw_str.append(timestamp).append("GET/users/self/verify");
    std::string payload =
        fmt::format(R"({{"op":"login","args":[{{"apiKey":"{}","passphrase":"{}","timestamp":"{}","sign":"{}"}}]}})",
                    account_secret_.api_key, account_secret_.api_phrase, timestamp,
                    generate_sign_hmac256_b64(account_secret_.api_secret, raw_str));
    if (index == 0) {
        send_ws_request(wss_api_, payload, "login_api");
    } else {
        send_ws_request(wss_stream_, payload, "login_stream");
    }
}

void OkxExecution::send_http_request(const HttpRequestBody& req, SpOrder order, OrderCallback cb,
                                     std::string_view name) {
    rest_.send(req, [this, order, cb, name](HttpResponseBody& res) {
        std::string response = boost::beast::buffers_to_string(res.body().data());
        do {
            if (res.result() != HTTP_STATUS_OK) {
                break;
            }
            try {
                PARSE_JSON(response, doc);
                if (doc["code"].error() != simdjson::SUCCESS) {
                    break;
                }
                std::string_view code = doc["code"];
                if (name == "place_order_rest") {
                    simdjson::dom::object data = *(doc["data"].begin());
                    std::string_view order_id = data["ordId"];
                    order->market_oid = order_id;
                    if (code == "0") {
                        order->status = OrderStatus::New;
                    } else {
                        break;
                    }
                } else if (name == "cancel_order_rest") {
                    if (code == "0") {
                        order->status = OrderStatus::Canceling;
                    } else {
                        break;
                    }
                } else if (name == "query_order") {
                    simdjson::dom::array datas = doc["data"];
                    for (auto item : datas) {
                        SpOrder rtn_order = parse_rtn_order(item);
                        order->update(*rtn_order);
                    }
                }
                INFRA_LOG_INFO("[okex] [{}] [success], recv: {}", name, response);
                order->milli = time_get_now_milli();
                cb(Errno::Ok, order);
                return;
            } catch (const std::exception& ex) {
                INFRA_LOG_WARN("[okex] [{}] [exception], msg: {}", name, ex.what());
            }
        } while (0);
        INFRA_LOG_WARN("[okex] [{}] [fail], recv: {}", name, response);
        order->ec = extract_error_msg(response);
        order->detail = response;
        order->status = OrderStatus::Failed;
        order->milli = time_get_now_milli();
        cb(order->ec, order);
    });
}

bool OkxExecution::convert_place_order(SpOrder order, OrderCallback cb, std::string& res, bool is_rest) {
    if (order->type != OrderType::Limit && order->type != OrderType::Market) {
        INFRA_LOG_WARN("[okex] [convert_place_order] [fail], msg: order type is not supported");
        cb(Errno::InvalidParams, order);
        return false;
    }

    if (order->client_oid.empty() || order->pair.empty()) {
        INFRA_LOG_WARN("[okex] [convert_place_order] [fail], msg: client_oid or pair is empty");
        cb(Errno::InvalidParams, order);
        return false;
    }

    auto it = g_pairs_info_cache.find(to_lower_str(order->pair));
    if (it == g_pairs_info_cache.end()) {
        INFRA_LOG_WARN("[okex] [convert_place_order] [fail], msg: not found {} in cache", order->pair);
        cb(Errno::InvalidParams, order);
        return false;
    }

    SpExPairInfo pair_info = it->second;
    std::map<std::string, std::string> params;
    bool reduce_only = false;

    params["tdMode"] = "cross";
    params["clOrdId"] = order->client_oid;
    std::string posSide{};
    if (order->side == OrderSide::OpenLong) {
        params["side"] = "buy";
        posSide = "long";
    } else if (order->side == OrderSide::OpenShort) {
        params["side"] = "sell";
        posSide = "short";
    } else if (order->side == OrderSide::CloseLong) {
        params["side"] = "sell";
        posSide = "long";
        if (g_current_position_mode == PositionMode::one_way_mode) {
            reduce_only = true;
        }
    } else if (order->side == OrderSide::CloseShort) {
        params["side"] = "buy";
        posSide = "short";
        if (g_current_position_mode == PositionMode::one_way_mode) {
            reduce_only = true;
        }
    }

    // NOTE：单向持仓模式不要填，双向持仓模式必填
    if (g_current_position_mode == PositionMode::hedge_mode) {
        params["posSide"] = posSide;
    }
    std::string tif = to_string(order->tif);
    if (order->tif == OrderTIF::MAKER) {
        tif = "post_only";
    }
    std::transform(tif.begin(), tif.end(), tif.begin(), ::tolower);
    double price = order->price;
    int price_decimals = static_cast<int>(std::round(-std::log10(pair_info->step_size_quote)));
    switch (order->type) {
        case OrderType::Limit:
            if (order->tif == OrderTIF::GTC) {
                params["ordType"] = "limit";
            } else {
                params["ordType"] = tif;
            }
                price = int(price / pair_info->step_size_quote) * pair_info->step_size_quote;
                params["px"] = fmt::format("{:.{}f}", price, price_decimals);
            break;
        case OrderType::Market:
            params["ordType"] = "market";
            break;
        default:
            INFRA_LOG_WARN("[okex] [convert_place_order] [fail], msg: order type is not supported");
            cb(Errno::InvalidParams, order);
            return false;
    }

    double quantity = order->quantity;
    quantity = int(quantity / pair_info->step_size_base) * pair_info->step_size_base;
    quantity /= get_denomination_value(order->pair); // 币数转合约张数
    int qty_decimals = static_cast<int>(std::round(-std::log10(pair_info->step_size_base)));
    params["sz"] = fmt::format("{:.{}f}", quantity, qty_decimals);
    

    if (is_rest) {
        params["instId"] = transfer_from_infra_pair(order->pair);
    } else {
        params["instIdCode"] = pair_info->alias;
    }

    std::string request_str{};
    request_str.reserve(256);
    request_str.append("{");
    for (const auto& [key, value] : params) {
        request_str.append("\"" + key + "\"").append(":").append("\"" + value + "\",");
    }
    if (reduce_only) {
        request_str.append("\"reduceOnly\":true,");
    }
    request_str.pop_back(); // remove last ','
    request_str.append("}");

    if (is_rest) {
        res = request_str;
    } else {
        res = fmt::format(R"({{"op":"order","id":"pOrder{}","args":[{}]}})", order->client_oid, request_str);
    }
    return true;
}

bool OkxExecution::send_ws_request(WebSocketClient& client, const std::string& content, const std::string& name) {
    if (client.is_socket_open()) {
        client.send(content);
        INFRA_LOG_INFO("[okex] [{}], send: {}", name, content);
        return true;
    } else {
        INFRA_LOG_WARN("[okex] [{}] [fail], msg: WebSocket not connected", name);
        return false;
    }
}

} // namespace infra
