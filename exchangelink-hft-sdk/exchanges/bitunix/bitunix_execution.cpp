#include "bitunix_execution.h"
using namespace infra::bitunix;

namespace infra {
bool BitunixExecution::initialize() {
    auto& info = g_config_map[base_config_.to_str()];
    if (info.empty()) {
        INFRA_LOG_WARN("[bitunix] [initialize] [fail], msg: {} {} {} not implemented",
                       to_string(base_config_.account_type), to_string(base_config_.address_type),
                       to_string(base_config_.settle_unit));
        return false;
    }

    if (account_secret_.api_key.empty() || account_secret_.api_secret.empty()) {
        INFRA_LOG_WARN("[bitunix] [initialize] [fail], msg: AccountSecret filed is empty");
        return false;
    }

    rest_host_ = info[REST_HOST];
    query_order_path_ = info[QUERY_ORDER_PATH_PATH];
    place_order_path_ = info[PLACE_ORDER_PATH_PATH];
    cancel_order_path_ = info[CANCEL_ORDER_PATH_PATH];

    wss_config_ = {info[WSS_PRIVATE_HOST], info[WSS_PORT], info[WSS_PRIVATE_PATH]};
    wss_stream_.resolve_connect(wss_config_.host, wss_config_.port, wss_config_.path);
    INFRA_LOG_INFO("[bitunix] [initialize] [Execution], websocket endpoint: {} {} {}", wss_config_.host,
                   wss_config_.path, wss_config_.port);
    return true;
}

void BitunixExecution::shutdown() { wss_stream_.close(); }

void BitunixExecution::query_order(const SpOrder order, OrderCallback cb) {
    if (order->market_oid.empty()) {
        INFRA_LOG_WARN("[bitunix] [query_order] [fail], msg: market_oid is empty");
        cb(Errno::InvalidParams, order);
        return;
    }

    std::string query{};
    query.append("orderId=").append(order->market_oid);
    auto req = get_request_body_with_sign(HTTP_GET, rest_host_, query_order_path_, query, "", account_secret_);
    send_http_request(req, order, cb, "query_order");
    INFRA_LOG_INFO("[bitunix] [query_order], send: {}", query);
}

void BitunixExecution::place_order_rest(const SpOrder order, OrderCallback cb) {
    std::string payload{};
    if (!convert_place_order(order, cb, payload)) {
        return;
    }

    auto req = get_request_body_with_sign(HTTP_POST, rest_host_, place_order_path_, "", payload, account_secret_);
    send_http_request(req, order, cb, "place_order_rest");
    this->add_order_cache(order);
    INFRA_LOG_INFO("[bitunix] [place_order_rest], send: {}", payload);
}

void BitunixExecution::cancel_order_rest(const SpOrder order, OrderCallback cb) {
    if (order->market_oid.empty() || order->pair.empty()) {
        INFRA_LOG_WARN("[bitunix] [cancel_order_rest] [fail], msg: market_oid or pair is empty");
        cb(Errno::InvalidParams, order);
        return;
    }

    Symbol symbol = transfer_from_infra_pair(order->pair);
    std::string payload =
        fmt::format(R"({{"symbol":"{}","orderList":[{{"orderId":"{}"}}]}})", symbol, order->market_oid);
    auto req = get_request_body_with_sign(HTTP_POST, rest_host_, cancel_order_path_, "", payload, account_secret_);
    send_http_request(req, order, cb, "cancel_order_rest");
    INFRA_LOG_INFO("[bitunix] [cancel_order_rest], send: {}", payload);
}

bool BitunixExecution::subscribe_order(OrderCallback cb) {
    this->order_handler_ = std::move(cb);
    std::string payload = R"({"op":"subscribe","args":[{"ch":"order"}]})";
    return wss_stream_.send(std::move(payload));
}

void BitunixExecution::unsubscribe_order() { this->order_handler_ = nullptr; }

void BitunixExecution::place_order_ws(const SpOrder order, OrderCallback cb) { place_order_rest(order, cb); }

void BitunixExecution::cancel_order_ws(const SpOrder order, OrderCallback cb) { cancel_order_rest(order, cb); }

Action BitunixExecution::on_connect(Wss* ws) {
    INFRA_LOG_INFO("[bitunix] [on_connect] [Execution], msg: WebSocket connection established");
    keep_ws_connection_alive();
    login();
    return Action::NONE;
}

Action BitunixExecution::on_ping(Wss* ws, std::string_view payload) {
    // INFRA_LOG_DEBUG("[bitunix] [on_ping] [Execution], payload: {}", payload);
    ws->pong(std::string(payload));
    return Action::NONE;
}

Action BitunixExecution::on_pong(Wss* ws, std::string_view payload) {
    // INFRA_LOG_DEBUG("[bitunix] [on_pong] [Execution], payload: {}", payload);
    return Action::NONE;
}

void BitunixExecution::on_close(Wss* ws) {
    INFRA_LOG_WARN("[bitunix] [on_close] [Execution], msg: WebSocket connection has been closed");
}

void BitunixExecution::on_error(Wss* ws, std::string_view err) {
    INFRA_LOG_WARN("[bitunix] [on_error] [Execution], msg: WebSocket error occurred: {}", err);
}

Action BitunixExecution::on_message(Wss* ws, std::string_view msg) {
    // INFRA_LOG_INFO("[bitunix] [on_message] [Execution], msg: {}", msg);
    try {
        PARSE_JSON(msg, doc);
        if (doc["ch"].error() == simdjson::SUCCESS) {
            std::string_view ch = doc["ch"];
            if (ch == "order") {
                simdjson::dom::object data = doc["data"];
                SpOrder rtn_order = parse_rtn_order(data);
                this->process_rtn_order(std::move(rtn_order));
                INFRA_LOG_INFO("[bitunix] [on_message] [order], recv: {}", msg);
            } else if (ch == "balance" || ch == "position") {
                // ignore
            } else {
                INFRA_LOG_WARN("[bitunix] [on_message] unexpected msg: {}", msg);
            }
        } else if (doc["op"].error() == simdjson::SUCCESS) {
            std::string_view op = doc["op"];
            if (op == "connect" || op == "login") {
                bool result = doc["data"]["result"];
                if (result) {
                    INFRA_LOG_INFO("[bitunix] [on_message] [Execution] [success], msg: {}", msg);
                } else {
                    INFRA_LOG_WARN("[bitunix] [on_message] [Execution] [fail], msg: {}", msg);
                }
            } else if (op == "ping") {
                // ignore
            } else {
                INFRA_LOG_WARN("[bitunix] [on_message], unexpected msg: {}", msg);
            }
        } else {
            INFRA_LOG_WARN("[bitunix] [on_message], unexpected msg: {}", msg);
        }
    } catch (const std::exception& ex) {
        INFRA_LOG_WARN("[bitunix] [on_message] [exception], error: {}, msg: {}", ex.what(), msg);
    }
    return Action::RECEIVE;
}

void BitunixExecution::login() {
    std::string timestamp = std::to_string(time_get_now_sec());
    std::string nonce = get_random_str(32);
    std::string digest_input = nonce + timestamp + account_secret_.api_key;
    std::string digest = generate_hash_sha256(digest_input);
    std::string sign_input = digest + account_secret_.api_secret;
    std::string signature = generate_hash_sha256(sign_input);
    std::string payload =
        fmt::format(R"({{"op":"login","args":[{{"apiKey":"{}","timestamp":{},"nonce":"{}","sign":"{}"}}]}})",
                    account_secret_.api_key, timestamp, nonce, signature);
    wss_stream_.send(std::move(payload));
}

void BitunixExecution::keep_ws_connection_alive() {
    std::string msg = fmt::format(R"({{"op":"ping","ping":{}}})", time_get_now_sec());
    wss_stream_.start_ping_pong(msg, 3);
}

bool BitunixExecution::convert_place_order(SpOrder order, OrderCallback cb, std::string& payload) {
    if (order->type != OrderType::Limit && order->type != OrderType::Market) {
        INFRA_LOG_WARN("[bitunix] [convert_place_order] [fail], msg: order type is not supported");
        cb(Errno::InvalidParams, order);
        return false;
    }

    if (order->client_oid.empty() || order->pair.empty()) {
        INFRA_LOG_WARN("[bitunix] [convert_place_order] [fail], msg: client_oid or pair is empty");
        cb(Errno::InvalidParams, order);
        return false;
    }

    auto it = g_pairs_info_cache.find(to_lower_str(order->pair));
    if (it == g_pairs_info_cache.end()) {
        INFRA_LOG_WARN("[bitunix] [place_ioc_order] [fail], msg: not found {} in cache", order->pair);
        cb(Errno::InvalidParams, order);
        return false;
    }

    SpExPairInfo pair_info = it->second;
    bfloat quantity = int(order->quantity / pair_info->step_size_base) * pair_info->step_size_base; // 调整数量精度
    bfloat price = int(order->price / pair_info->step_size_quote) * pair_info->step_size_quote;     // 调整价格精度

    constexpr std::array<const char*, 5> tifToStr = {"GTC", "POST_ONLY", "IOC", "FOK", "POC"};
    std::string_view tifStr = tifToStr[static_cast<uint8_t>(order->tif)];
    std::string_view orderType = (order->type == OrderType::Limit) ? "LIMIT" : "MARKET";
    std::string_view side =
        (order->side == OrderSide::OpenLong || order->side == OrderSide::CloseShort) ? "BUY" : "SELL";
    std::string_view tradeSide =
        (order->side == OrderSide::OpenLong || order->side == OrderSide::OpenShort) ? "OPEN" : "CLOSE";
    bool reduce = (order->side == OrderSide::OpenLong || order->side == OrderSide::OpenShort) ? false : true;

    if (g_current_position_mode == PositionMode::one_way_mode) {
        payload = fmt::format(
            R"({{"symbol":"{}","orderType":"{}","side":"{}","reduceOnly":{},"effect":"{}","qty":"{}","price":"{}","clientId":"{}"}})",
            transfer_from_infra_pair(order->pair), orderType, side, reduce, tifStr, (quantity.str()), (price.str()),
            order->client_oid);
    } else {
        payload = fmt::format(
            R"({{"symbol":"{}","orderType":"{}","side":"{}","tradeSide":"{}","effect":"{}","qty":"{}","price":"{}","clientId":"{}"}})",
            transfer_from_infra_pair(order->pair), orderType, side, tradeSide, tifStr, (quantity.str()), (price.str()),
            order->client_oid);
    }
    return true;
}

void BitunixExecution::send_http_request(const HttpRequestBody& req, SpOrder order, OrderCallback cb,
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
                    order->market_oid = doc["data"]["orderId"];
                    order->status = OrderStatus::New;
                } else if (name == "cancel_order_rest") {
                    order->status = OrderStatus::Canceling;
                } else if (name == "query_order") {
                    simdjson::dom::object obj = doc["data"];
                    SpOrder rtn_order = parse_rtn_order(obj, true);
                    order->update(*rtn_order);
                }
                INFRA_LOG_INFO("[bitunix] [{}] [success], recv: {}", name, response);
                order->milli = time_get_now_milli();
                cb(Errno::Ok, order);
                return;
            } catch (const std::exception& ex) {
                INFRA_LOG_WARN("[bitunix] [{}], exception: {}", name, ex.what());
            }
        } while (0);
        INFRA_LOG_WARN("[bitunix] [{}] [fail], recv: {}", name, response);
        order->ec = extract_error_code(response);
        order->detail = response;
        order->status = OrderStatus::Failed;
        order->milli = time_get_now_milli();
        cb(order->ec, order);
    });
}
} // namespace infra