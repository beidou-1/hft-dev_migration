#include "gate_execution.h"
using namespace infra::gate;

namespace infra {
bool GateExecution::initialize() {
    auto& info = g_config_map[base_config_.to_str()];
    if (info.empty()) {
        INFRA_LOG_WARN("[gate] [initialize] [fail], msg: {} {} {} not implemented",
                       to_string(base_config_.account_type), to_string(base_config_.address_type),
                       to_string(base_config_.settle_unit));
        return false;
    }

    rest_host_ = info[REST_HOST];
    order_path_ = info[ORDER_PATH_PATH];

    if (account_secret_.api_key.empty() || account_secret_.api_secret.empty()) {
        INFRA_LOG_WARN("[gate] [initialize] [fail], msg: AccountSecret filed is empty");
        return false;
    }

    wss_config_ = {info[WSS_PRIVATE_HOST], info[WSS_PORT], info[WSS_PRIVATE_PATH]};
    wss_stream_.set_user_data(0);
    wss_stream_.set_ws_header_field(std::bind(&GateExecution::add_header, this, std::placeholders::_1));
    wss_stream_.resolve_connect(wss_config_.host, wss_config_.port, wss_config_.path);
    INFRA_LOG_INFO("[gate] [initialize] [Execution], websocket endpoint: {} {} {}", wss_config_.host, wss_config_.path,
                   wss_config_.port);

    wss_trade_config_ = {info[WSS_TRADE_HOST], info[WSS_PORT], info[WSS_TRADE_PATH]};
    wss_trade_.set_user_data(1);
    wss_trade_.set_ws_header_field(std::bind(&GateExecution::add_header, this, std::placeholders::_1));
    wss_trade_.resolve_connect(wss_trade_config_.host, wss_trade_config_.port, wss_trade_config_.path);
    INFRA_LOG_INFO("[gate] [initialize] [Execution], websocket endpoint: {} {} {}", wss_trade_config_.host,
                   wss_trade_config_.path, wss_trade_config_.port);
    return true;
}

void GateExecution::add_header(websocket::request_type& req) { req.set("X-Gate-Size-Decimal", "1"); }

void GateExecution::shutdown() {
    unsubscribe_order();
    wss_trade_.close();
}

bool GateExecution::subscribe_order(OrderCallback cb) {
    this->order_handler_ = std::move(cb);
    std::string timestamp = std::to_string(time_get_now_sec());
    std::string sign = get_websocket_sign("futures.orders", "subscribe", timestamp, account_secret_);
    std::string payload = fmt::format(
        R"({{"time":{},"channel":"futures.orders","event":"subscribe","payload":["20011","!all"],"auth":{{"method":"api_key","KEY":"{}","SIGN":"{}"}}}})",
        timestamp, account_secret_.api_key, sign);
    INFRA_LOG_INFO("[gate] [subscribe_order], send: {}", payload);
    return send_ws_request(wss_stream_, payload, "subscribe_order");
}

void GateExecution::unsubscribe_order() {
    this->order_handler_ = nullptr;
    std::string timestamp = std::to_string(time_get_now_sec());
    std::string sign = get_websocket_sign("futures.orders", "unsubscribe", timestamp, account_secret_);
    std::string payload = fmt::format(
        R"({{"time":{},"channel":"futures.orders","event":"unsubscribe","payload":["20011","!all"],"auth":{{"method":"api_key","KEY":"{}","SIGN":"{}"}}}})",
        timestamp, account_secret_.api_key, sign);
    send_ws_request(wss_stream_, payload, "unsubscribe_order");
    INFRA_LOG_INFO("[gate] [unsubscribe_order] [success]");
}

void GateExecution::place_order(const SpOrder order, OrderCallback cb) {
    std::string payload{};
    auto it = g_pairs_info_cache.find(order->pair);
    if (it == g_pairs_info_cache.end()) [[unlikely]] {
        INFRA_LOG_WARN("[gate] [place_order] [fail], msg: pair {} not found in cache", order->pair);
        cb(Errno::InvalidParams, order);
        return;
    }

    const auto& pair_info = it->second;

    int side = (order->side == OrderSide::OpenLong || order->side == OrderSide::CloseShort) ? 1 : -1;
    bool reduce_only = (order->side == OrderSide::CloseLong || order->side == OrderSide::CloseShort);

    if (order->type == OrderType::Limit && order->tif != OrderTIF::IOC && order->tif != OrderTIF::FOK &&
        order->tif != OrderTIF::GTC) {
        INFRA_LOG_WARN("[gate] [place_order] [fail], msg: unsupported tif for limit order");
        cb(Errno::InvalidParams, order);
        return;
    }

    const char* tif_str = "gtc";
    if (order->type == OrderType::Market || order->tif == OrderTIF::IOC)
        tif_str = "ioc";
    else if (order->tif == OrderTIF::FOK)
        tif_str = "fok";

    double price = int(order->price / pair_info->step_size_quote) * pair_info->step_size_quote;
    double quantity = int(order->quantity / pair_info->step_size_base) * pair_info->step_size_base;
    quantity = side * (quantity / pair_info->denomination_value);

    int price_decimals = static_cast<int>(std::round(-std::log10(pair_info->step_size_quote)));

    std::string dynamic_parts;
    if (order->type == OrderType::Market) {
        dynamic_parts += R"("price":"0")";
    } else {
        dynamic_parts += fmt::format(R"("price":"{:.{}f}")", price, price_decimals);
    }

    if (reduce_only) {
        dynamic_parts += R"(,"reduce_only":true)";
    }

    payload = fmt::format(R"({{"contract":"{}",{},"size":"{}","tif":"{}","text":"t-{}"}})",
                          transfer_from_infra_pair(order->pair), dynamic_parts, quantity, tif_str, order->client_oid);

    std::string timestamp = std::to_string(time_get_now_sec());
    order->uid = generate_req_id();
    std::string req_id = std::to_string(order->uid);
    std::string ws_msg = fmt::format(
        R"({{"time":{},"channel":"futures.order_place","event":"api","payload":{{"req_id":"{}","req_param":{}}}}})",
        timestamp, req_id, payload);

    INFRA_LOG_INFO("[gate] [place_order], send: {}", ws_msg);
    send_ws_request(wss_trade_, ws_msg, "place_order");
    ws_request_cache_[order->uid] = std::make_pair(order, cb);
}

void GateExecution::cancel_order(const SpOrder order, OrderCallback cb) {
    std::string timestamp = std::to_string(time_get_now_sec());
    order->uid = generate_req_id();
    std::string req_id = std::to_string(order->uid);
    std::string payload = fmt::format(R"({{"order_id":"{}"}})", order->market_oid);
    std::string ws_msg = fmt::format(
        R"({{"time":{},"channel":"futures.order_cancel","event":"api","payload":{{"req_id":"{}","req_param":{}}}}})",
        timestamp, req_id, payload);
    INFRA_LOG_INFO("[gate] [cancel_order], send: {}", ws_msg);
    send_ws_request(wss_trade_, ws_msg, "cancel_order");
    ws_request_cache_[order->uid] = std::make_pair(order, cb);
}

void GateExecution::query_order(const SpOrder order, OrderCallback cb) {
    if (order->market_oid.empty()) {
        INFRA_LOG_WARN("[gate] [query_order] [fail], msg: market_oid is empty");
        cb(Errno::InvalidParams, order);
        return;
    }
    INFRA_LOG_INFO("[gate] [query_order], market_oid: {}", order->market_oid);
    auto req = get_request_body_with_sign(HTTP_GET, rest_host_, order_path_ + "/" + order->market_oid, "", "",
                                          account_secret_);
    send_http_request(req, order, cb, "query_order");
}

bool GateExecution::send_ws_request(WebSocketClient& client, const std::string& content, const std::string& name) {
    if (client.is_socket_open()) {
        client.send(content);
        return true;
    }
    INFRA_LOG_WARN("[gate] [{}] [fail], msg: WebSocket not connected", name);
    return false;
}

Action GateExecution::on_connect(Wss* ws) {
    size_t index = ws->get_index();
    INFRA_LOG_INFO("[gate] [on_connect] [Execution], connection id: {}", index);
    login(index);
    return Action::NONE;
}

Action GateExecution::on_ping(Wss* ws, std::string_view payload) {
    ws->pong(std::string(payload));
    return Action::NONE;
}

Action GateExecution::on_pong(Wss* ws, std::string_view payload) { return Action::NONE; }

void GateExecution::on_close(Wss* ws) {
    size_t index = ws->get_index();
    INFRA_LOG_WARN("[gate] [on_close] [Execution], connection id: {}", index);
}

void GateExecution::on_error(Wss* ws, std::string_view err) {
    size_t index = ws->get_index();
    INFRA_LOG_WARN("[gate] [on_error] [Execution], connection id: {}, err: {}", index, err);
}

Action GateExecution::on_message(Wss* ws, std::string_view msg) {
    try {
        PARSE_JSON(msg, doc);
        if (doc["header"].error() == simdjson::SUCCESS) {
            std::string_view channel = doc["header"]["channel"];
            std::string_view rt_code = doc["header"]["status"].get_string().value();
            std::string_view req_id_text = doc["request_id"];
            unsigned long uid = std::stoul(std::string(req_id_text));

            if (channel == "futures.order_place" || channel == "futures.order_cancel") {
                auto iter = ws_request_cache_.find(uid);
                if (iter == ws_request_cache_.end()) {
                    INFRA_LOG_WARN("[gate] [on_message] [{}] [fail], uid not found, recv: {}", channel, msg);
                    return Action::RECEIVE;
                }

                auto [order, cb] = iter->second;
                if (rt_code != "200" || doc["data"]["errs"].error() == simdjson::SUCCESS) {
                    INFRA_LOG_WARN("[gate] [on_message] [{}] [fail], recv: {}", channel, msg);
                    order->ec = extract_error_code(msg);
                    order->detail = msg;
                    order->status = OrderStatus::Failed;
                    order->milli = time_get_now_milli();
                    cb(order->ec, order);
                    return Action::RECEIVE;
                }

                if (doc["ack"].error() == simdjson::NO_SUCH_FIELD) {
                    if (channel == "futures.order_place") {
                        order->market_oid = std::to_string(doc["data"]["result"]["id"].get_int64());
                        order->status = OrderStatus::New;
                        order->exchange_create_time =
                            static_cast<int64_t>(doc["data"]["result"]["create_time"].get_double() * 1000.0);
                        INFRA_LOG_INFO("[gate] [place_order] [success], recv: {}", msg);
                    } else {
                        order->status = OrderStatus::Canceling;
                        INFRA_LOG_INFO("[gate] [cancel_order] [success], recv: {}", msg);
                    }
                    order->milli = time_get_now_milli();
                    cb(Errno::Ok, order);
                    ws_request_cache_.erase(iter);
                }
            } else if (channel == "futures.login") {
                INFRA_LOG_INFO("[gate] [login], recv: {}", msg);
            } else {
                INFRA_LOG_WARN("[gate] [on_message] [Execution], unexpected msg: {}", msg);
            }
        } else if (doc["channel"].error() == simdjson::SUCCESS && doc["event"].error() == simdjson::SUCCESS) {
            std::string_view channel = doc["channel"];
            std::string_view event = doc["event"];
            if (channel == "futures.orders" && event == "update") {
                simdjson::dom::array data_list = doc["result"];
                for (auto&& item : data_list) {
                    SpOrder rtn_order = parse_rtn_order(item, channel);
                    this->dispatch_order(std::move(rtn_order));
                }
                INFRA_LOG_INFO("[gate] [subscribe_order], recv: {}", msg);
            } else if (event == "subscribe" || event == "unsubscribe") {
                std::string_view status = doc["result"]["status"];
                if (status == "success") {
                    INFRA_LOG_INFO("[gate] [{}] [success], recv: {}", event, msg);
                } else {
                    INFRA_LOG_WARN("[gate] [{}] [fail], recv: {}", event, msg);
                }
            } else {
                INFRA_LOG_WARN("[gate] [on_message] [Execution], unexpected msg: {}", msg);
            }
        } else {
            INFRA_LOG_WARN("[gate] [on_message] [Execution], unexpected msg: {}", msg);
        }
    } catch (const std::exception& ex) {
        INFRA_LOG_WARN("[gate] [on_message] [Execution], msg: {}, ex: {}", ex.what(), msg);
    }
    return Action::RECEIVE;
}

void GateExecution::login(size_t index) {
    std::string timestamp = std::to_string(time_get_now_sec());
    std::string req_id = std::to_string(generate_req_id());
    std::string sign = get_ws_api_sign("futures.login", "", timestamp, account_secret_);
    std::string payload =
        fmt::format(R"({{"api_key":"{}","signature":"{}","timestamp":"{}","req_id":"{}","request_param":""}})",
                    account_secret_.api_key, sign, timestamp, req_id);
    std::string login_msg =
        fmt::format(R"({{"time":{},"channel":"futures.login","event":"api","payload":{}}})", timestamp, payload);

    if (index == 0)
        send_ws_request(wss_stream_, login_msg, "login");
    else if (index == 1)
        send_ws_request(wss_trade_, login_msg, "login");
}

void GateExecution::send_http_request(const HttpRequestBody& req, SpOrder order, OrderCallback cb,
                                      std::string_view name) {
    client_.send(req, [this, order, cb, name](HttpResponseBody& res) {
        std::string msg = boost::beast::buffers_to_string(res.body().data());
        do {
            auto status = res.result_int();
            if (status != 200 && status != 201)
                break;
            try {
                PARSE_JSON(msg, doc);
                if (doc["id"].error() != simdjson::SUCCESS)
                    break;
                if (name == "place_order_rest") {
                    order->market_oid = std::to_string(doc["id"].get_uint64().value());
                    std::string_view order_status_text = doc["status"];
                    int64_t left = doc["left"].get_int64().value();
                    if (order_status_text == "finished" && left != 0)
                        order->status = OrderStatus::Canceled;
                    else if (order_status_text == "finished" && left == 0)
                        order->status = OrderStatus::Filled;
                    else
                        order->status = OrderStatus::New;
                    order->exchange_create_time = static_cast<int64_t>(doc["create_time"].get_double() * 1000.0);
                    order->exchange_update_time = static_cast<int64_t>(doc["update_time"].get_double() * 1000.0);
                } else if (name == "cancel_order_rest") {
                    order->status = OrderStatus::Canceling;
                } else if (name == "query_order") {
                    SpOrder rtn_order = parse_rtn_order(doc, name);
                    order->update(*rtn_order);
                }
                INFRA_LOG_INFO("[gate] [{}] [success], recv: {}", name, msg);
                order->milli = time_get_now_milli();
                cb(Errno::Ok, order);
                return;
            } catch (const std::exception& ex) {
                INFRA_LOG_WARN("[gate] [{}] [exception], ex: {}", name, ex.what());
            }
        } while (0);
        INFRA_LOG_WARN("[gate] [{}] [fail], recv: {}", name, msg);
        order->ec = extract_error_code(msg);
        order->detail = msg;
        order->status = OrderStatus::Failed;
        order->milli = time_get_now_milli();
        cb(order->ec, order);
    });
}
} // namespace infra