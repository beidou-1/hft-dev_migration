#include "hyperliquid_execution.h"
using namespace infra::hyperliquid;

namespace infra {
bool HyperliquidExecution::initialize() {
    auto& info = g_config_map[base_config_.to_str()];
    if (info.empty()) {
        INFRA_LOG_WARN("[hyperliquid] [initialize] [fail], msg: {} {} {} not implemented",
                       to_string(base_config_.account_type), to_string(base_config_.address_type),
                       to_string(base_config_.settle_unit));
        return false;
    }

    if (account_secret_.api_secret.empty() || account_secret_.wallet_address.empty()) {
        INFRA_LOG_WARN("[hyperliquid] [initialize] [fail], msg: AccountSecret filed is empty");
        return false;
    }

    rest_host_ = info[REST_HOST];
    query_order_path_ = info[QUERY_ORDER_PATH_PATH];
    place_order_path_ = info[PLACE_ORDER_PATH_PATH];
    cancel_order_path_ = info[CANCEL_ORDER_PATH_PATH];

    wss_config_ = {info[WSS_PRIVATE_HOST], info[WSS_PORT], info[WSS_PRIVATE_PATH]};
    wss_stream_.resolve_connect(wss_config_.host, wss_config_.port, wss_config_.path);
    INFRA_LOG_INFO("[hyperliquid] [initialize] [Execution], websocket endpoint: {} {} {}", wss_config_.host,
                   wss_config_.path, wss_config_.port);
    return true;
}

void HyperliquidExecution::shutdown() { wss_stream_.close(); }

void HyperliquidExecution::query_order(const SpOrder& order, OrderCallback cb) {
    if (order->market_oid.empty()) {
        INFRA_LOG_WARN("[hyperliquid] [query_order] [fail], msg: market_oid is empty");
        cb(Errno::InvalidParams, order);
        return;
    }

    std::string query = fmt::format(R"({{"type":"orderStatus","user":"{}","oid":{}}})", account_secret_.wallet_address,
                                    order->market_oid);
    auto req = get_request_body_by_post(rest_host_, query_order_path_, query);
    send_http_request(req, order, cb, "query_order");
    INFRA_LOG_INFO("[hyperliquid] [query_order], send: {}", query);
}

bool HyperliquidExecution::subscribe_order(OrderCallback cb) {
    this->order_handler_ = std::move(cb);
    std::string payload = R"({"method":"subscribe","subscription":{"type":"orderUpdates","user":")" +
                          account_secret_.wallet_address + R"("}})";
    INFRA_LOG_INFO("[hyperliquid] [subscribe_order], send: {}", payload);
    send_ws_request(std::move(payload));
    return true;
}

void HyperliquidExecution::unsubscribe_order() {
    this->order_handler_ = nullptr;
    std::string payload = R"({"method":"unsubscribe","subscription":{"type":"orderUpdates","user":")" +
                          account_secret_.wallet_address + R"("}})";
    INFRA_LOG_INFO("[hyperliquid] [unsubscribe_order], send: {}", payload);
    send_ws_request(std::move(payload));
}

void HyperliquidExecution::place_order(const SpOrder& order, OrderCallback cb) {
    std::string payload{};

    Symbol pair = transfer_from_infra_pair(order->pair);
    auto it = g_pairs_info_cache.find(pair);
    if (it == g_pairs_info_cache.end()) {
        INFRA_LOG_WARN("[hyperliquid] [convert_place_order] [fail], msg: not found {} in cache", pair);
        order->ec = Errno::InvalidParams;
        order->detail = "pair not found in cache";
        order->status = OrderStatus::Failed;
        cb(Errno::InvalidParams, order);
        return;
    }

    SpExPairInfo pair_info = it->second;
    double quantity = int(order->quantity / pair_info->step_size_base) * pair_info->step_size_base; // 调整数量精度
    double price = int(order->price / pair_info->step_size_quote) * pair_info->step_size_quote;     // 调整价格精度
    bool isBuy = (order->side == OrderSide::OpenLong || order->side == OrderSide::CloseShort) ? true : false;
    bool reduceOnly = (order->side == OrderSide::CloseLong || order->side == OrderSide::CloseShort) ? true : false;

    constexpr std::array<const char*, 5> tifToStr = {"Gtc", "Alo", "Ioc", "Fok", "Poc"};
    std::string_view tifStr = tifToStr[static_cast<uint8_t>(order->tif)];

    int asset_id = std::stoi(pair_info->alias);
    std::string action_str{};
    if (order->type == OrderType::Limit) {
        action_str = fmt::format(
            R"({{"type":"order","orders":[{{"a":{},"b":{},"p":"{}","s":"{}","r":{},"t":{{"limit":{{"tif":"{}"}}}},"c":"{}"}}],"grouping":"na"}})",
            asset_id, isBuy, std::to_string(price), std::to_string(quantity), reduceOnly, tifStr,
            transfer_oid(order->client_oid));
    } else {
        INFRA_LOG_WARN("[hyperliquid] [convert_place_order] [fail], msg: {} type is not supported",
                       to_string(order->type));
        cb(Errno::InvalidParams, order);
        return;
    }

    int64_t nonce = time_get_now_milli();
    EcdsaSignature sign;
    sign_action(nonce, action_str, account_secret_, sign);
    payload = fmt::format(R"({{"action":{},"nonce":{},"signature":{{"r":"{}","s":"{}","v":{}}}}})", action_str, nonce,
                          sign.r_hex, sign.s_hex, sign.v);
    auto tmp_uid = generate_req_id();
    std::string request_body =
        fmt::format(R"({{"method":"post","id":{},"request":{{"type":"action","payload":{}}}}})", tmp_uid, payload);
    INFRA_LOG_INFO("[hyperliquid] [place_order], send: {}", request_body);
    send_ws_request(std::move(request_body));
    ws_request_cache_[tmp_uid] = std::make_pair(order, cb);
}

void HyperliquidExecution::cancel_order(const SpOrder& order, OrderCallback cb) {

    Symbol pair = transfer_from_infra_pair(order->pair);
    auto it = g_pairs_info_cache.find(pair);
    if (it == g_pairs_info_cache.end()) {
        INFRA_LOG_WARN("[hyperliquid] [convert_cancel_order] [fail], msg: not found {} in cache", pair);
        cb(Errno::InvalidParams, order);
        return;
    }

    SpExPairInfo pair_info = it->second;
    int asset_id = std::stoi(pair_info->alias);
    std::string action_str =
        fmt::format(R"({{"type":"cancel","cancels":[{{"a":{},"o":{}}}]}})", asset_id, order->market_oid);
    int64_t nonce = time_get_now_milli();

    EcdsaSignature sign;
    sign_action(nonce, action_str, account_secret_, sign);

    std::string payload = fmt::format(R"({{"action":{},"nonce":{},"signature":{{"r":"{}","s":"{}","v":{}}}}})",
                                      action_str, nonce, sign.r_hex, sign.s_hex, sign.v);
    auto tmp_uid = generate_req_id();
    std::string request_body =
        fmt::format(R"({{"method":"post","id":{},"request":{{"type":"action","payload":{}}}}})", tmp_uid, payload);
    INFRA_LOG_INFO("[hyperliquid] [cancel_order], send: {}", request_body);
    send_ws_request(std::move(request_body));
    ws_request_cache_[tmp_uid] = std::make_pair(order, cb);
}

Action HyperliquidExecution::on_connect(Wss* ws) {
    INFRA_LOG_INFO("[hyperliquid] [on_connect] [Execution], msg: WebSocket connection established");
    keep_ws_connection_alive();
    login();
    return Action::NONE;
}

Action HyperliquidExecution::on_ping(Wss* ws, std::string_view payload) {
    // INFRA_LOG_DEBUG("[hyperliquid] [on_ping] [Execution], payload: {}", payload);
    ws->pong(std::string(payload));
    return Action::NONE;
}

Action HyperliquidExecution::on_pong(Wss* ws, std::string_view payload) {
    // INFRA_LOG_DEBUG("[hyperliquid] [on_pong] [Execution], payload: {}", payload);
    return Action::NONE;
}

void HyperliquidExecution::on_close(Wss* ws) {
    INFRA_LOG_WARN("[hyperliquid] [on_close] [Execution], msg: WebSocket connection has been closed");
}

void HyperliquidExecution::on_error(Wss* ws, std::string_view err) {
    INFRA_LOG_WARN("[hyperliquid] [on_error] [Execution], msg: WebSocket error occurred: {}", err);
}

Action HyperliquidExecution::on_message(Wss* ws, std::string_view msg) {
    // INFRA_LOG_DEBUG("[hyperliquid] [on_message] [Execution], msg: {}", msg);
    try {
        PARSE_JSON(msg, doc);
        if (doc["channel"].error() == simdjson::SUCCESS) {
            std::string_view channel = doc["channel"];
            if (channel == "orderUpdates") {
                simdjson::dom::array data_list = doc["data"];
                for (auto data : data_list) {
                    SpOrder rtn_order = parse_rtn_order(data);
                    this->dispatch_order(std::move(rtn_order));
                }
                INFRA_LOG_INFO("[hyperliquid] [on_message] [order], recv: {}", msg);
            } else if (channel == "subscriptionResponse") {
                INFRA_LOG_INFO("[hyperliquid] [on_message] [subscribe], msg: {}", msg);
            } else if (channel == "post") {
                // 处理逻辑参考send_http_request
                int64_t id = doc["data"]["id"];
                auto iter = ws_request_cache_.find(id);
                if (iter == ws_request_cache_.end()) {
                    INFRA_LOG_WARN("[hyperliquid] [on_message] [post] [fail], msg:", msg);
                    return Action::RECEIVE;
                }

                auto [order, cb] = ws_request_cache_[id];
                simdjson::dom::object payload = doc["data"]["response"]["payload"];
                std::string_view status = payload["status"];
                if (status != "ok") {
                    INFRA_LOG_WARN("[hyperliquid] [on_message] [post] [fail], msg: {}", msg);
                    order->ec = extract_error_code(msg);
                    order->detail = msg;
                    order->status = OrderStatus::Failed;
                    order->milli = time_get_now_milli();
                    cb(order->ec, order);
                    ws_request_cache_.erase(iter);
                    return Action::RECEIVE;
                }

                std::string_view type = payload["response"]["type"];
                if (type == "order") {
                    simdjson::dom::array array = payload["response"]["data"]["statuses"];
                    simdjson::dom::object item = *(array.begin());
                    if (item["error"].error() == simdjson::SUCCESS) {
                        INFRA_LOG_WARN("[hyperliquid] [on_message] [post] [fail], msg: {}", msg);
                        std::string json_str = simdjson::minify(item);
                        order->ec = extract_error_code(json_str);
                        order->detail = json_str;
                        order->status = OrderStatus::Failed;
                        order->milli = time_get_now_milli();
                        cb(order->ec, order);
                        ws_request_cache_.erase(iter);
                        return Action::RECEIVE;
                    } else if (item["filled"].error() == simdjson::SUCCESS) { // 立即成交
                        int64_t oid = item["filled"]["oid"].get_int64();
                        order->market_oid = std::to_string(oid);
                        order->status = OrderStatus::PartiallyFilled;
                        std::string_view totalSz = item["filled"]["totalSz"];
                        std::string_view avgPx = item["filled"]["avgPx"];
                        order->cum_deal_base = str_to_float(totalSz);
                        order->avg_price = str_to_float(avgPx);
                        order->cum_deal_quote = order->cum_deal_base * order->avg_price;
                    } else {
                        int64_t oid = item["resting"]["oid"].get_int64();
                        order->market_oid = std::to_string(oid);
                        order->status = OrderStatus::New;
                    }
                    INFRA_LOG_INFO("[hyperliquid] [place_order_ws] [success], recv: {}", msg);
                } else if (type == "cancel") {
                    order->status = OrderStatus::Canceling;
                    INFRA_LOG_INFO("[hyperliquid] [cancel_order_ws] [success], recv: {}", msg);
                } else {
                    INFRA_LOG_WARN("[hyperliquid] [on_message] [post] [fail], msg: {}", msg);
                }

                order->milli = time_get_now_milli();
                cb(Errno::Ok, order);
                ws_request_cache_.erase(iter);
            } else if (channel == "pong") {
                // ignore
            } else {
                INFRA_LOG_WARN("[hyperliquid] [on_message] unexpected msg: {}", msg);
            }
        } else {
            INFRA_LOG_WARN("[hyperliquid] [on_message] unexpected msg: {}", msg);
        }
    } catch (const std::exception& ex) {
        INFRA_LOG_WARN("[hyperliquid] [on_message] [exception], error: {}, msg: {}", ex.what(), msg);
    }
    return Action::RECEIVE;
}

void HyperliquidExecution::login() {
    // TODO
}

void HyperliquidExecution::keep_ws_connection_alive() {
    wss_stream_.start_ping_pong(R"({"method":"ping"})", 30); // 心跳检测时间为60秒
}

void HyperliquidExecution::send_http_request(const HttpRequestBody& req, SpOrder order, OrderCallback cb,
                                             std::string_view name) {
    rest_.send(req, [this, order, cb, name](HttpResponseBody& res) {
        std::string response = boost::beast::buffers_to_string(res.body().data());
        do {
            if (res.result() != HTTP_STATUS_OK) {
                break;
            }
            try {
                PARSE_JSON(response, doc);
                std::string_view status = doc["status"];
                if (status != "ok" && status != "order") {
                    break;
                }
                if (name == "place_order") {
                    simdjson::dom::array array = doc["response"]["data"]["statuses"];
                    simdjson::dom::object item = *(array.begin());
                    if (item["error"].error() == simdjson::SUCCESS) {
                        break;
                    } else if (item["filled"].error() == simdjson::SUCCESS) { // 立即成交
                        int64_t oid = item["filled"]["oid"].get_int64();
                        order->market_oid = std::to_string(oid);
                        order->status = OrderStatus::PartiallyFilled;
                        std::string_view totalSz = item["filled"]["totalSz"];
                        std::string_view avgPx = item["filled"]["avgPx"];
                        order->cum_deal_base = str_to_float(totalSz);
                        order->avg_price = str_to_float(avgPx);
                        order->cum_deal_quote = order->cum_deal_base * order->avg_price;
                    } else {
                        int64_t oid = item["resting"]["oid"].get_int64();
                        order->market_oid = std::to_string(oid);
                        order->status = OrderStatus::New;
                    }
                } else if (name == "cancel_order") {
                    if (response.find("success") != std::string_view::npos) {
                        order->status = OrderStatus::Canceling;
                    } else {
                        simdjson::dom::array array = doc["response"]["data"]["statuses"];
                        simdjson::dom::object item = array.at(0);
                        if (item["error"].error() == simdjson::SUCCESS) {
                            break;
                        }
                    }
                } else if (name == "query_order") {
                    simdjson::dom::object obj = doc["order"];
                    SpOrder rtn_order = parse_rtn_order(obj);
                    order->update(*rtn_order);
                }
                INFRA_LOG_INFO("[hyperliquid] [{}] [success], recv: {}", name, response);
                order->milli = time_get_now_milli();
                cb(Errno::Ok, order);
                return;
            } catch (const std::exception& ex) {
                INFRA_LOG_WARN("[hyperliquid] [{}], exception: {}", name, ex.what());
            }
        } while (0);
        INFRA_LOG_WARN("[hyperliquid] [{}] [fail], recv: {}", name, response);
        order->ec = extract_error_code(response);
        order->detail = response;
        order->status = OrderStatus::Failed;
        order->milli = time_get_now_milli();
        cb(order->ec, order);
    });
}

void HyperliquidExecution::send_ws_request(std::string&& content) {
    if (wss_stream_.is_socket_open()) {
        wss_stream_.send(std::move(content));
    } else {
        INFRA_LOG_WARN("[hyperliquid] [send_ws_request] [fail], msg: WebSocket not connected");
    }
}
} // namespace infra