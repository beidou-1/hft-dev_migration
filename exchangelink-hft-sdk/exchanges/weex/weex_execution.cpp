#include "weex_execution.h"
using namespace infra::weex;

namespace infra {
bool WeexExecution::initialize() {
    auto& info = g_config_map[base_config_.to_str()];
    if (info.empty()) {
        INFRA_LOG_WARN("[weex] [initialize] [fail], msg: {} {} {} not implemented",
                       to_string(base_config_.account_type), to_string(base_config_.address_type),
                       to_string(base_config_.settle_unit));
        return false;
    }

    if (account_secret_.api_key.empty() || account_secret_.api_secret.empty() || account_secret_.api_phrase.empty()) {
        INFRA_LOG_WARN("[weex] [initialize] [fail], msg: AccountSecret field is empty");
        return false;
    }

    rest_host_ = info[REST_HOST];
    query_order_path_ = info[QUERY_ORDER_PATH_PATH];
    place_order_path_ = info[PLACE_ORDER_PATH_PATH];
    cancel_order_path_ = info[CANCEL_ORDER_PATH_PATH];

    wss_config_ = {info[WSS_PRIVATE_HOST], info[WSS_PORT], info[WSS_PRIVATE_PATH]};
    wss_stream_.set_sign_cb(std::bind(&WeexExecution::sign_ws, this, std::placeholders::_1));
    wss_stream_.resolve_connect(wss_config_.host, wss_config_.port, wss_config_.path);
    INFRA_LOG_INFO("[weex] [initialize] [Execution], websocket endpoint: {} {} {}", wss_config_.host, wss_config_.path,
                   wss_config_.port);
    return true;
}

void WeexExecution::shutdown() { wss_stream_.close(); }

void WeexExecution::query_order(const SpOrder order, OrderCallback cb) {
    if (order->market_oid.empty() && order->client_oid.empty()) {
        INFRA_LOG_WARN("[weex] [query_order] [fail], msg: order id is empty");
        cb(Errno::InvalidParams, order);
        return;
    }

    std::string query = "orderId=" + order->market_oid;
    auto req = get_request_body_with_sign(HTTP_GET, rest_host_, query_order_path_, query, "", account_secret_);
    send_http_request(req, order, cb, "query_order");
}

void WeexExecution::place_order_rest(const SpOrder order, OrderCallback cb) {
    std::string payload;
    if (!convert_place_order(order, cb, payload))
        return;

    auto req = get_request_body_with_sign(HTTP_POST, rest_host_, place_order_path_, "", payload, account_secret_);
    this->add_order_cache(order);
    send_http_request(req, order, cb, "place_order_rest");
    INFRA_LOG_INFO("[weex] [place_order_rest], send: {}", payload);
}

void WeexExecution::cancel_order_rest(const SpOrder order, OrderCallback cb) {
    if (order->market_oid.empty()) {
        INFRA_LOG_WARN("[weex] [cancel_order_rest] [fail], msg: market_oid or pair is empty");
        cb(Errno::InvalidParams, order);
        return;
    }

    std::string query = "orderId=" + order->market_oid;
    auto req = get_request_body_with_sign(HTTP_DELETE, rest_host_, cancel_order_path_, query, "", account_secret_);
    send_http_request(req, order, cb, "cancel_order_rest");
    INFRA_LOG_INFO("[weex] [cancel_order_rest], send: {}", query);
}

bool WeexExecution::subscribe_order(OrderCallback cb) {
    this->order_handler_ = std::move(cb);
    std::string payload = R"({"id":101,"method":"SUBSCRIBE","params":["orders"]})";
    return wss_stream_.send(std::move(payload));
}

void WeexExecution::unsubscribe_order() {
    this->order_handler_ = nullptr;
    std::string payload = R"({"id":101,"method":"UNSUBSCRIBE","params":["orders"]})";
    wss_stream_.send(std::move(payload));
}

void WeexExecution::place_order_ws(const SpOrder order, OrderCallback cb) { place_order_rest(order, cb); }

void WeexExecution::cancel_order_ws(const SpOrder order, OrderCallback cb) { cancel_order_rest(order, cb); }

Action WeexExecution::on_connect(Wss* ws) {
    INFRA_LOG_INFO("[weex] [on_connect] [Execution], msg: WebSocket connection established");
    return Action::NONE;
}

Action WeexExecution::on_ping(Wss* ws, std::string_view payload) {
    ws->pong(std::string(payload));
    return Action::NONE;
}

Action WeexExecution::on_pong(Wss* ws, std::string_view payload) { return Action::NONE; }

void WeexExecution::on_close(Wss* ws) {
    INFRA_LOG_WARN("[weex] [on_close] [Execution], msg: WebSocket connection has been closed");
}

void WeexExecution::on_error(Wss* ws, std::string_view err) {
    INFRA_LOG_WARN("[weex] [on_error] [Execution], msg: WebSocket error occurred: {}", err);
}

Action WeexExecution::on_message(Wss* ws, std::string_view msg) {
    // INFRA_LOG_INFO("[weex] [on_message] [Execution], msg: {}", msg);
    try {
        PARSE_JSON(msg, doc);
        if (doc["e"].error() == simdjson::SUCCESS) {
            std::string_view event = doc["e"];
            if (event == "orders") {
                simdjson::dom::array array = doc["d"];
                for (auto item : array) {
                    SpOrder rtn_order = parse_rtn_order(item);
                    this->process_rtn_order(std::move(rtn_order));
                }
                INFRA_LOG_INFO("[weex] [on_message] [order], recv: {}", msg);
            } else {
                INFRA_LOG_WARN("[weex] [on_message] [Execution] unexpected msg: {}", msg);
            }
        } else if (doc["event"].error() == simdjson::SUCCESS) {
            std::string_view event = doc["event"];
            if (event == "connected") {
                INFRA_LOG_INFO("[weex] [on_message] [Execution] recv: {}", msg);
            } else {
                INFRA_LOG_WARN("[weex] [on_message] [Execution] unexpected msg: {}", msg);
            }
        } else if (doc["type"].error() == simdjson::SUCCESS) {
            std::string_view type = doc["type"];
            if (type == "ping") {
                std::string pong_msg = R"({"method":"PONG","id":101})";
                wss_stream_.send(std::move(pong_msg));
            } else if (type == "cash-gift-event" || type == "coupon-event" || type == "trade-event") {
                INFRA_LOG_INFO("[weex] [on_message] [Execution] recv: {}", msg);
            } else {
                INFRA_LOG_INFO("[weex] [on_message] [Execution] unexpected msg:: {}", msg);
            }
        } else if (doc["result"].error() == simdjson::SUCCESS) {
            bool result = doc["result"];
            if (result) {
                INFRA_LOG_INFO("[weex] [on_message] [Execution] recv: {}", msg);
            } else {
                INFRA_LOG_WARN("[weex] [on_message] [Execution] recv: {}", msg);
            }
        } else {
            INFRA_LOG_WARN("[weex] [on_message] [Execution] unexpected msg: {}", msg);
        }
    } catch (const std::exception& ex) {
        INFRA_LOG_WARN("[weex] [on_message] [Execution] [exception], error: {}, msg: {}", ex.what(), msg);
    }
    return Action::RECEIVE;
}

// NOTE: 通过wss请求头做认证
void WeexExecution::sign_ws(boost::beast::websocket::request_type& req) {
    std::string timestamp = std::to_string(time_get_now_milli());
    std::string msg = timestamp + wss_config_.path;
    std::string signature = generate_sign_hmac256_b64(account_secret_.api_secret, msg);

    req.set("ACCESS-KEY", account_secret_.api_key);
    req.set("ACCESS-SIGN", signature);
    req.set("ACCESS-PASSPHRASE", account_secret_.api_phrase);
    req.set("ACCESS-TIMESTAMP", timestamp);
}

bool WeexExecution::convert_place_order(SpOrder order, OrderCallback cb, std::string& payload) {
    if (order->type != OrderType::Limit && order->type != OrderType::Market) {
        INFRA_LOG_WARN("[weex] [convert_place_order] [fail], msg: order type not supported");
        cb(Errno::InvalidParams, order);
        return false;
    }

    if (order->client_oid.empty() || order->pair.empty()) {
        INFRA_LOG_WARN("[weex] [convert_place_order] [fail], msg: client_oid or pair is empty");
        cb(Errno::InvalidParams, order);
        return false;
    }

    auto it = g_pairs_info_cache.find(to_lower_str(order->pair));
    if (it == g_pairs_info_cache.end()) {
        INFRA_LOG_WARN("[weex] [convert_place_order] [fail], msg: {} not found in cache", order->pair);
        cb(Errno::InvalidParams, order);
        return false;
    }

    SpExPairInfo pair_info = it->second;
    bfloat quantity = int(order->quantity / pair_info->step_size_base) * pair_info->step_size_base;
    bfloat price = int(order->price / pair_info->step_size_quote) * pair_info->step_size_quote;

    std::string_view side, positionSide;
    if (order->side == OrderSide::OpenLong) {
        side = "BUY";
        positionSide = "LONG";
    } else if (order->side == OrderSide::CloseLong) {
        side = "SELL";
        positionSide = "LONG";
    } else if (order->side == OrderSide::OpenShort) {
        side = "SELL";
        positionSide = "SHORT";
    } else if (order->side == OrderSide::CloseShort) {
        side = "BUY";
        positionSide = "SHORT";
    }

    constexpr std::array<const char*, 5> tifToStr = {"GTC", "POST_ONLY", "IOC", "FOK", "POC"};
    std::string_view tif_str = tifToStr[static_cast<uint8_t>(order->tif)];
    std::string_view type = (order->type == OrderType::Limit) ? "LIMIT" : "MARKET";

    payload = fmt::format(
        R"({{"symbol":"{}","type":"{}","side":"{}","positionSide":"{}","timeInForce":"{}","quantity":"{}","price":"{}","newClientOrderId":"{}"}})",
        transfer_from_infra_pair(order->pair), type, side, positionSide, tif_str, quantity.str(), price.str(),
        order->client_oid);
    return true;
}

void WeexExecution::send_http_request(const HttpRequestBody& req, SpOrder order, OrderCallback cb,
                                      std::string_view name) {
    rest_.send(req, [this, order, cb, name](HttpResponseBody& res) {
        std::string response = boost::beast::buffers_to_string(res.body().data());
        do {
            if (res.result() != HTTP_STATUS_OK)
                break;
            try {
                PARSE_JSON(response, doc);
                if (doc["code"].error() == simdjson::SUCCESS && doc["code"].get_string() != SUCCESS_CODE)
                    break;

                if (name == "place_order_rest") {
                    order->market_oid = doc["orderId"];
                    order->status = OrderStatus::New;
                } else if (name == "cancel_order_rest") {
                    order->status = OrderStatus::Canceling;
                } else if (name == "query_order") {
                    simdjson::dom::object obj = doc.get_object();
                    SpOrder rtn_order = parse_rtn_order(obj, true);
                    order->update(*rtn_order);
                }
                INFRA_LOG_INFO("[weex] [{}] [success], recv: {}", name, response);
                order->milli = time_get_now_milli();
                cb(Errno::Ok, order);
                return;
            } catch (const std::exception& ex) {
                INFRA_LOG_WARN("[weex] [{}] [exception], msg: {}", name, ex.what());
            }
        } while (0);
        INFRA_LOG_WARN("[weex] [{}] [fail], recv: {}", name, response);
        order->ec = extract_error_code(response);
        order->detail = response;
        order->status = OrderStatus::Failed;
        order->milli = time_get_now_milli();
        cb(order->ec, order);
    });
}
} // namespace infra
