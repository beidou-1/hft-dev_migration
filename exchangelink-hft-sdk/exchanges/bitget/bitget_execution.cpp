#include "bitget_execution.h"
#include "exchanges/signature.h"
using namespace infra::bitget;

namespace infra {
bool BitgetExecution::initialize() {
    auto& info = g_config_map[base_config_.to_str()];
    if (info.empty()) {
        INFRA_LOG_WARN("[bitget] [initialize] [fail], msg: {} {} {} not implemented",
                       to_string(base_config_.account_type), to_string(base_config_.address_type),
                       to_string(base_config_.settle_unit));
        return false;
    }

    rest_host_ = info[REST_HOST];
    query_order_path_ = info[QUERY_ORDER_PATH_PATH];
    place_order_path_ = info[PLACE_ORDER_PATH_PATH];
    cancel_order_path_ = info[CANCEL_ORDER_PATH_PATH];

    if (account_secret_.api_key.empty() || account_secret_.api_secret.empty() || account_secret_.api_phrase.empty()) {
        INFRA_LOG_WARN("[bitget] [initialize] [fail], msg: AccountSecret filed is empty");
        return false;
    }

    wss_config_ = {info[WSS_PRIVATE_HOST], info[WSS_PORT], info[WSS_PRIVATE_PATH]};
    wss_stream_.set_user_data(0);
    wss_stream_.resolve_connect(wss_config_.host, wss_config_.port, wss_config_.path);
    INFRA_LOG_INFO("[bitget] [initialize] [Execution], websocket endpoint: {} {} {}", wss_config_.host,
                   wss_config_.path, wss_config_.port);

    wss_trade_config_ = {info[WSS_PRIVATE_HOST], info[WSS_PORT], info[WSS_PRIVATE_PATH]};
    wss_trade_.set_user_data(1);
    wss_trade_.resolve_connect(wss_trade_config_.host, wss_trade_config_.port, wss_trade_config_.path);
    INFRA_LOG_INFO("[bitget] [initialize] [Execution], websocket endpoint: {} {} {}", wss_trade_config_.host,
                   wss_trade_config_.path, wss_trade_config_.port);

    ws_request_cache_.reserve(1024);
    return true;
}

void BitgetExecution::shutdown() {
    unsubscribe_order();
    wss_trade_.close();
}

bool BitgetExecution::subscribe_order(OrderCallback cb) {
    this->order_handler_ = std::move(cb);
    std::string payload = R"({"op":"subscribe","args":[{"instType":"UTA","topic":"order"}]})";
    wss_stream_.send(payload);
    INFRA_LOG_INFO("[bitget] [subscribe_order], send: {}", payload);
    return true;
}

void BitgetExecution::unsubscribe_order() {
    this->order_handler_ = nullptr;
    wss_stream_.close();
    INFRA_LOG_INFO("[bitget] [unsubscribe_order] [success]");
}

void BitgetExecution::place_order(const SpOrder order, OrderCallback cb) {
    auto uid = generate_req_id();
    auto it = g_pairs_info_cache.find(order->pair);
    if (it == g_pairs_info_cache.end()) [[unlikely]] {
        INFRA_LOG_WARN("[bitget] [place_order] [fail] msg: pair {} not found in cache, cache size: {}", order->pair,
                       g_pairs_info_cache.size());
        order->ec = Errno::InvalidParams;
        order->detail = "pair not found in cache";
        order->status = OrderStatus::Failed;
        cb(order->ec, order);
        return;
    }

    const auto& info = it->second;
    double quantity = std::floor(order->quantity / info->step_size_base) * info->step_size_base;
    if (quantity < info->trading_min_base) [[unlikely]] {
        INFRA_LOG_WARN("[bitget] [place_order] [fail] msg: order too small: {} < {}", quantity, info->trading_min_base);
        order->ec = Errno::InvalidParams;
        order->detail = "order quantity too small";
        order->status = OrderStatus::Failed;
        cb(order->ec, order);
        return;
    }

    double price{0};
    size_t offset_reduce{0}, offset_side{0};
    switch (order->side) {
        case OrderSide::OpenLong:
            price = std::floor(order->price / info->step_size_quote) * info->step_size_quote;
            break;
        case OrderSide::OpenShort:
            price = std::ceil(order->price / info->step_size_quote) * info->step_size_quote;
            offset_side = 1;
            break;
        case OrderSide::CloseShort:
            price = std::floor(order->price / info->step_size_quote) * info->step_size_quote;
            offset_reduce = 1;
            break;
        case OrderSide::CloseLong:
            price = std::ceil(order->price / info->step_size_quote) * info->step_size_quote;
            offset_reduce = 1;
            offset_side = 1;
            break;
        default:
            break;
    }

    size_t offset_type = (order->type == OrderType::Limit) ? 0 : 1;
    size_t idx = offset_reduce + (offset_side << 1) + (offset_type << 2);

    json_cache_.set_buf(wss_trade_.get_buf());
    json_cache_.append(FIXED_FIELD);
    json_cache_.append(filed_array[idx]);
    json_cache_.append(CID_FIELD);
    json_cache_.append(order->client_oid);
    json_cache_.append(PAIR_FIELD);
    json_cache_.append(info->alias);
    json_cache_.append(QTY_FIELD);
    json_cache_.append(quantity);
    json_cache_.append(PRICE_FIELD);
    json_cache_.append(price);
    json_cache_.append(UID_FIELD);
    json_cache_.append(uid);
    json_cache_.append(END_FIELD);

    // order->latency->master_order.serial_tsc = rdtsc();
    wss_trade_.send_buf(json_cache_.get_size());
    std::string_view payload = json_cache_.get_json();
    INFRA_LOG_INFO("[bitget] [place_order], send: {}", payload);
    ws_request_cache_.try_emplace(uid, order->pair, order->client_oid, std::move(cb));
}

void BitgetExecution::cancel_order(const SpOrder order, OrderCallback cb) {
    auto uid = generate_req_id();
    std::string payload = fmt::format(
        R"({{"op":"trade","topic":"cancel-order","id":"{}","category":"usdt-futures","args":[{{"orderId":"{}"}}]}})",
        uid, order->market_oid);
    INFRA_LOG_INFO("[bitget] [cancel_order], send: {}", payload);
    wss_trade_.send(std::move(payload));
    ws_request_cache_.try_emplace(uid, order->pair, order->client_oid, std::move(cb));
}

void BitgetExecution::query_order(const SpOrder order, OrderCallback cb) {
    std::string query = "orderId=" + order->market_oid;
    auto req = get_request_body_with_sign(HTTP_GET, rest_host_, query_order_path_, query, account_secret_);
    client_.send(req, [this, order, cb](HttpResponseBody& res) {
        std::string msg = boost::beast::buffers_to_string(res.body().data());
        do {
            if (res.result() != HTTP_STATUS_OK) {
                break;
            }
            try {
                PARSE_JSON(msg, doc);
                if (doc["data"].error() == simdjson::SUCCESS && !doc["data"].is_null()) {
                    simdjson::dom::object obj = doc["data"];
                    auto rtn_order = parse_rtn_order(obj);
                    order->update(*rtn_order.get());
                    cb(Errno::Ok, order);
                    return;
                }
            } catch (const std::exception& ex) {
                INFRA_LOG_WARN("[bitget] [query_order] [exception], ex: {}", ex.what());
            }
        } while (0);
        INFRA_LOG_WARN("[bitget] [query_order] [fail], recv: {}", msg);
        order->ec = extract_error_code(msg);
        order->detail = msg;
        order->status = OrderStatus::Failed;
        order->milli = time_get_now_milli();
        cb(order->ec, order);
    });
}

Action BitgetExecution::on_connect(Wss* ws) {
    size_t index = ws->get_index();
    INFRA_LOG_INFO("[bitget] [on_connect] [Execution], connection id: {}", index);
    login(index);
    keep_ws_connection_alive(index);
    return Action::NONE;
}

Action BitgetExecution::on_ping(Wss* ws, std::string_view payload) {
    ws->pong(std::string(payload));
    return Action::NONE;
}

Action BitgetExecution::on_pong(Wss* ws, std::string_view payload) { return Action::NONE; }

void BitgetExecution::on_close(Wss* ws) {
    size_t index = ws->get_index();
    INFRA_LOG_WARN("[bitget] [on_close] [Execution], connection id: {}", index);
}

void BitgetExecution::on_error(Wss* ws, std::string_view err) {
    size_t index = ws->get_index();
    INFRA_LOG_WARN("[bitget] [on_error] [Execution], connection id: {}, err: {}", index, err);
}

Action BitgetExecution::on_message(Wss* ws, std::string_view msg) {
    char first = msg[0];
    if (first != '{') [[unlikely]] { // 过滤非json响应，例如"pong"
        return Action::RECEIVE;
    }
    try {
        PARSE_JSON(msg, doc);
        if (doc["data"].error() == simdjson::SUCCESS) {
            INFRA_LOG_INFO("[bitget] [subscribe_order], recv: {}", msg);
            on_message_stream(doc);
        } else if (doc["event"].error() == simdjson::SUCCESS) {
            std::string_view event = doc["event"];
            if (event == "trade") {
                std::string_view req_id = doc["id"];
                bool response_ok = false;
                if (doc["code"].error() == simdjson::SUCCESS) {
                    if (doc["code"].get_string() == WS_SUCCESS_CODE)
                        response_ok = true;
                }
                std::string_view topic = doc["topic"];
                if (topic == "place-order" || topic == "cancel-order") {
                    auto order = std::make_shared<Order>();
                    if (response_ok) {
                        simdjson::dom::object obj = doc["args"].at(0); // 只取一个
                        order->market_oid = obj["orderId"];
                        if (topic == "place-order") {
                            order->status = OrderStatus::New;
                            INFRA_LOG_INFO("[bitget] [place_order] [success], recv: {}", msg);
                        } else {
                            order->status = OrderStatus::Canceling;
                            INFRA_LOG_INFO("[bitget] [cancel_order] [success], recv: {}", msg);
                        }
                        order->milli = time_get_now_milli();
                        process_ws_response(req_id, order);
                    } else {
                        Errno ec = extract_error_code(msg);
                        order->status = OrderStatus::Failed;
                        order->detail = msg;
                        order->ec = ec;
                        INFRA_LOG_WARN("[bitget] [on_message] [{}] [fail], recv: {}", topic, msg);
                        process_ws_response(req_id, order);
                    }
                }
            } else if (event == "error") {
                std::string_view req_id = doc["id"];
                auto order = std::make_shared<Order>();
                order->ec = extract_error_code(msg);
                order->detail = msg;
                order->status = OrderStatus::Failed;
                INFRA_LOG_WARN("[bitget] [on_message] [fail], recv: {}", msg);
                process_ws_response(req_id, order);
            } else if (event == "subscribe" || event == "login") {
                INFRA_LOG_INFO("[bitget] [{}] [success], recv: {}", event, msg);
            } else {
                INFRA_LOG_WARN("[bitget] [on_message] [Execution], unexpected msg: {}", msg);
            }
        } else {
            INFRA_LOG_WARN("[bitget] [on_message] [Execution], unexpected msg: {}", msg);
        }
    } catch (const std::exception& ex) {
        INFRA_LOG_WARN("[bitget] [on_message] [Execution], msg: {}, ex: {}", ex.what(), msg);
    }
    return Action::RECEIVE;
}

void BitgetExecution::login(size_t index) {
    std::string timestamp = std::to_string(time_get_now_sec());
    std::string msg = timestamp + "GET/user/verify";
    std::string signature = generate_sign_hmac256_b64(account_secret_.api_secret, msg);

    std::string payload =
        fmt::format(R"({{"op":"login","args":[{{"apiKey":"{}","passphrase":"{}","timestamp":"{}","sign":"{}"}}]}})",
                    account_secret_.api_key, account_secret_.api_phrase, timestamp, signature);
    (index == 0 ? wss_stream_ : wss_trade_).send(payload);
}

void BitgetExecution::keep_ws_connection_alive(size_t index) {
    if (index == 0) {
        wss_stream_.start_ping_pong("ping", 30);
    } else {
        wss_trade_.start_ping_pong("ping", 1); // 提高ping的频率，有助于降低发送延迟
    }
}

void BitgetExecution::on_message_stream(const simdjson::dom::element& doc) {
    simdjson::dom::array data = doc["data"];
    for (auto&& item : data) {
        SpOrder rtn_order = parse_rtn_order(item);
        this->dispatch_order(rtn_order);
    }
}

void BitgetExecution::process_ws_response(std::string_view id, SpOrder rtn_order) {
    int64_t uid = std::stol(std::string(id));
    auto iter = ws_request_cache_.find(uid);
    if (iter == ws_request_cache_.end()) {
        INFRA_LOG_WARN("[bitget] [process_ws_response], msg: not found cache by {}", uid);
        return;
    }

    auto& cache = iter->second;
    rtn_order->pair = cache.pair;
    rtn_order->client_oid = cache.client_oid;
    cache.cb(rtn_order->ec, rtn_order);
    ws_request_cache_.erase(iter);
}
} // namespace infra