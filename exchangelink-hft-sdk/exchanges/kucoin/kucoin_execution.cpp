#include "kucoin_execution.h"
#include <boost/asio/steady_timer.hpp>
using namespace infra::kucoin;

namespace infra {
bool KucoinExecution::initialize() {
    if (account_secret_.api_key.empty() || account_secret_.api_secret.empty() || account_secret_.api_phrase.empty()) {
        INFRA_LOG_WARN("[kucoin] [initialize] [fail], msg: AccountSecret filed is empty");
        return false;
    }

    get_account_mode(); // 调用接口，根据api-key判断当前账户是经典模式还是统一模式

    std::string key;
    if (g_account_mode == AccountMode::CLASSIC) {
        key = "classic";
    } else {
        key = base_config_.to_str();
    }

    auto& info = g_config_map[key];
    if (info.empty()) {
        INFRA_LOG_WARN("[kucoin] [initialize] [fail], msg: {} {} {} {} not implemented",
                       to_string(base_config_.account_type), to_string(base_config_.address_type),
                       to_string(base_config_.settle_unit), to_string(base_config_.account_mode));
        return false;
    }

    rest_host_ = info[REST_HOST];
    order_path_ = info[ORDER_PATH_PATH];
    cancel_order_path_ = info[CANCEL_ORDER_PATH_PATH];
    query_order_path_ = info[QUERY_ORDER_PATH_PATH];

    auto private_token = get_private_token();
    std::string private_path{};
    if (g_account_mode == AccountMode::CLASSIC) {
        private_path = fmt::format(info[WSS_PRIVATE_PATH], private_token, time_get_now_micro());
    } else if (g_account_mode == AccountMode::UNIFIED) {
        private_path = fmt::format(info[WSS_PRIVATE_PATH], private_token);
    }

    wss_config_ = {info[WSS_PRIVATE_HOST], info[WSS_PORT], private_path};
    wss_stream_.set_user_data(0);
    wss_stream_.resolve_connect(wss_config_.host, wss_config_.port, wss_config_.path);
    INFRA_LOG_INFO("[kucoin] [initialize] [Execution], stream websocket endpoint: {} {} {}", wss_config_.host,
                   wss_config_.path, wss_config_.port);

    std::string trade_path = get_ws_url(account_secret_);
    wss_trade_config_ = {info[WSS_TRADE_HOST], info[WSS_PORT], trade_path};
    wss_trade_.set_user_data(1);
    wss_trade_.disable_auto_reconnect();
    wss_trade_.resolve_connect(wss_trade_config_.host, wss_trade_config_.port, wss_trade_config_.path);
    INFRA_LOG_INFO("[kucoin] [initialize] [Execution], trade websocket endpoint: {} {} {}", wss_trade_config_.host,
                   wss_trade_config_.path, wss_trade_config_.port);
    return true;
}

void KucoinExecution::shutdown() { wss_stream_.close(); }

std::string KucoinExecution::get_private_token() {
    std::string path = g_account_mode == AccountMode::CLASSIC ? "/api/v1/bullet-private" : "/api/v2/bullet-private";
    auto req = get_request_body_with_sign(HTTP_POST, rest_host_, path, "", account_secret_);
    boost::beast::error_code ec;
    std::string response = rest_.sync_send(req, ec);
    do {
        if (ec) {
            break;
        }
        try {
            PARSE_JSON(response, doc);
            if (doc["code"].get_string()->compare(KUCOIN_SUCCESS_CODE) != 0) {
                break;
            }
            INFRA_LOG_INFO("[kucoin] [get_private_token] [success], recv: {}", response);
            return std::string(doc["data"]["token"]);
        } catch (const std::exception& ex) {
            INFRA_LOG_WARN("[kucoin] [get_private_token] [exception], msg: {}", ex.what());
        }
    } while (0);
    INFRA_LOG_WARN("[kucoin] [get_private_token] [fail], recv: {}", response);
    return "";
}

void KucoinExecution::query_order(const SpOrder order, OrderCallback cb) {
    if (g_account_mode == AccountMode::CLASSIC) {
        query_classic_order(order, cb);
    } else if (g_account_mode == AccountMode::UNIFIED) {
        query_unified_order(order, cb);
    }
}
void KucoinExecution::query_classic_order(const SpOrder order, OrderCallback cb) {
    if (order->market_oid.empty()) {
        INFRA_LOG_WARN("[kucoin] [query_order] [fail], msg: market_oid is empty");
        cb(Errno::InvalidParams, order);
        return;
    }

    std::string query_order_path = fmt::format(query_order_path_, order->market_oid);
    auto req = get_request_body_with_sign(HTTP_GET, rest_host_, query_order_path, "", account_secret_);
    send_http_request(req, order, cb, "query_order");
    INFRA_LOG_INFO("[kucoin] [query_order], send: {}", order->market_oid);
}

void KucoinExecution::query_unified_order(const SpOrder order, OrderCallback cb) {
    if (order->market_oid.empty()) {
        INFRA_LOG_WARN("[kucoin] [query_order] [fail], msg: market_oid is empty");
        cb(Errno::InvalidParams, order);
        return;
    }

    std::string query =
        fmt::format("tradeType=FUTURES&symbol={}&orderId={}", transfer_from_infra_pair(order->pair), order->market_oid);
    auto req = get_request_body_with_sign(HTTP_GET, rest_host_, query_order_path_, query, account_secret_);
    send_http_request(req, order, cb, "query_order");
    INFRA_LOG_INFO("[kucoin] [query_order], send: {}", query);
}

void KucoinExecution::place_order_rest(const SpOrder order, OrderCallback cb) {
    if (g_account_mode == AccountMode::CLASSIC) {
        place_classic_order_rest(order, cb);
    } else if (g_account_mode == AccountMode::UNIFIED) {
        place_unified_order_rest(order, cb);
    }
}
void KucoinExecution::place_classic_order_rest(const SpOrder order, OrderCallback cb) {
    std::string payload{};
    if (!convert_place_classic_order(order, cb, payload)) {
        return;
    }

    auto req = get_request_body_with_sign(HTTP_POST, rest_host_, order_path_, payload, account_secret_);
    send_http_request(req, order, cb, "place_order_rest");
    INFRA_LOG_INFO("[kucoin] [place_order_rest], send: {}", payload);
}

void KucoinExecution::place_unified_order_rest(const SpOrder order, OrderCallback cb) {
    std::string payload{};
    if (!convert_place_unified_order(order, cb, payload)) {
        return;
    }

    auto req = get_request_body_with_sign(HTTP_POST, rest_host_, order_path_, payload, account_secret_);
    send_http_request(req, order, cb, "place_order_rest");
    INFRA_LOG_INFO("[kucoin] [place_order_rest], send: {}", payload);
}

void KucoinExecution::cancel_order_rest(const SpOrder order, OrderCallback cb) {
    if (g_account_mode == AccountMode::CLASSIC) {
        cancel_classic_order_rest(order, cb);
    } else if (g_account_mode == AccountMode::UNIFIED) {
        cancel_unified_order_rest(order, cb);
    }
}
void KucoinExecution::cancel_classic_order_rest(const SpOrder order, OrderCallback cb) {
    if (order->market_oid.empty()) {
        INFRA_LOG_WARN("[kucoin] [cancel_order_rest] [fail], msg: market_oid is empty");
        cb(Errno::InvalidParams, order);
        return;
    }

    std::string cancel_order_path = fmt::format(cancel_order_path_, order->market_oid);
    auto req = get_request_body_with_sign(HTTP_DELETE, rest_host_, cancel_order_path, "", account_secret_);
    send_http_request(req, order, cb, "cancel_order_rest");
    INFRA_LOG_INFO("[kucoin] [cancel_order_rest], send: {}", order->market_oid);
}

void KucoinExecution::cancel_unified_order_rest(const SpOrder order, OrderCallback cb) {
    if (order->market_oid.empty()) {
        INFRA_LOG_WARN("[kucoin] [cancel_order_rest] [fail], msg: market_oid is empty");
        cb(Errno::InvalidParams, order);
        return;
    }

    std::string exchange_symbol = transfer_from_infra_pair(order->pair);
    std::string payload =
        fmt::format(R"({{"symbol":"{}","tradeType":"FUTURES","orderId":"{}"}})", exchange_symbol, order->market_oid);
    auto req = get_request_body_with_sign(HTTP_POST, rest_host_, cancel_order_path_, payload, account_secret_);
    send_http_request(req, order, cb, "cancel_order_rest");
    INFRA_LOG_INFO("[kucoin] [cancel_order_rest], send: {}", order->market_oid);
}

bool KucoinExecution::subscribe_order(OrderCallback cb) {
    this->order_handler_ = std::move(cb);
    std::string payload{};
    if (g_account_mode == AccountMode::CLASSIC) {
        payload = fmt::format(
            R"({{"id":{},"type":"subscribe","topic":"/contractMarket/tradeOrders","response":true,"privateChannel":"true"}})",
            time_get_now_micro());
    } else if (g_account_mode == AccountMode::UNIFIED) {
        payload = fmt::format(R"({{"id":"{}","action":"SUBSCRIBE","channel":"orderAll","tradeType":"UNIFIED"}})",
                              time_get_now_micro());
    }
    return send_ws_request(wss_stream_, payload, "subscribe_order");
}

void KucoinExecution::unsubscribe_order() {
    this->order_handler_ = nullptr;
    std::string payload{};
    if (g_account_mode == AccountMode::CLASSIC) {
        payload = fmt::format(
            R"({{"id":{},"type":"unsubscribe","topic":"/contractMarket/tradeOrders","response":true,"privateChannel":"true"}})",
            time_get_now_micro());
    } else if (g_account_mode == AccountMode::UNIFIED) {
        payload = fmt::format(R"({{"id":"{}","action":"UNSUBSCRIBE","channel":"orderAll","tradeType":"UNIFIED"}})",
                              time_get_now_micro());
    }
    send_ws_request(wss_stream_, payload, "unsubscribe_order");
    INFRA_LOG_INFO("[kucoin] [unsubscribe_order] [success]");
}

void KucoinExecution::place_order_ws(const SpOrder order, OrderCallback cb) {
    if (g_account_mode == AccountMode::CLASSIC) {
        place_classic_order_ws(order, cb);
    } else if (g_account_mode == AccountMode::UNIFIED) {
        place_unified_order_ws(order, cb);
    }
}

void KucoinExecution::place_classic_order_ws(const SpOrder order, OrderCallback cb) {
    std::string args{};
    if (!convert_place_classic_order(order, cb, args)) {
        return;
    }
    order->uid = generate_req_id();
    std::string payload = fmt::format(R"({{"id":"{}","op":"futures.order","args":{}}})", order->uid, args);
    send_ws_request(wss_trade_, payload, "place_order_ws");
    ws_request_cache_[order->uid] = std::make_pair(order, cb);
}

void KucoinExecution::place_unified_order_ws(const SpOrder order, OrderCallback cb) {
    std::string args{};
    if (!convert_place_unified_order(order, cb, args)) {
        return;
    }
    order->uid = generate_req_id();
    std::string payload = fmt::format(R"({{"id":"{}","op":"uta.order","args":{}}})", order->uid, args);
    send_ws_request(wss_trade_, payload, "place_order_ws");
    ws_request_cache_[order->uid] = std::make_pair(order, cb);
}

void KucoinExecution::cancel_order_ws(const SpOrder order, OrderCallback cb) {
    if (g_account_mode == AccountMode::CLASSIC) {
        cancel_classic_order_ws(order, cb);
    } else if (g_account_mode == AccountMode::UNIFIED) {
        cancel_unified_order_ws(order, cb);
    }
}

void KucoinExecution::cancel_classic_order_ws(const SpOrder order, OrderCallback cb) {
    if (order->market_oid.empty() || order->pair.empty()) {
        INFRA_LOG_WARN("[kucoin] [{}] [cancel_order_ws] [fail], msg: market_oid or pair is empty",
                       to_string(base_config_.account_type));
        cb(Errno::InvalidParams, order);
        return;
    }

    std::string exchange_symbol = transfer_from_infra_pair(order->pair);
    std::string args =
        fmt::format(R"({{"symbol":"{}","tradeType":"FUTURES","orderId":"{}"}})", exchange_symbol, order->market_oid);
    order->uid = generate_req_id();
    std::string payload = fmt::format(R"({{"id":"{}","op":"futures.cancel","args":{}}})", order->uid, args);
    this->send_ws_request(wss_trade_, payload, "cancel_order_ws");
    ws_request_cache_[order->uid] = std::make_pair(order, cb);
}

void KucoinExecution::cancel_unified_order_ws(const SpOrder order, OrderCallback cb) {
    if (order->market_oid.empty() || order->pair.empty()) {
        INFRA_LOG_WARN("[kucoin] [{}] [cancel_order_ws] [fail], msg: market_oid or pair is empty",
                       to_string(base_config_.account_type));
        cb(Errno::InvalidParams, order);
        return;
    }

    std::string exchange_symbol = transfer_from_infra_pair(order->pair);
    std::string args =
        fmt::format(R"({{"symbol":"{}","tradeType":"FUTURES","orderId":"{}"}})", exchange_symbol, order->market_oid);
    order->uid = generate_req_id();
    std::string payload = fmt::format(R"({{"id":"{}","op":"uta.cancel","args":{}}})", order->uid, args);
    this->send_ws_request(wss_trade_, payload, "cancel_order_ws");
    ws_request_cache_[order->uid] = std::make_pair(order, cb);
}

Action KucoinExecution::on_connect(Wss* ws) {
    size_t index = ws->get_index();
    INFRA_LOG_INFO("[kucoin] [on_connect] [Execution], msg: WebSocket connection established, connection ID: {}",
                   index);
    if (g_account_mode == AccountMode::CLASSIC && index == 0) {
        keep_ws_connection_alive(wss_stream_);
    } else if (g_account_mode == AccountMode::CLASSIC && index == 1) {
        keep_private_ws_connection_alive(wss_trade_);
    } else if (g_account_mode == AccountMode::UNIFIED && index == 0) {
        keep_ws_connection_alive(wss_stream_);
    } else if (g_account_mode == AccountMode::UNIFIED && index == 1) {
        keep_private_ws_connection_alive(wss_trade_);
    }
    return Action::NONE;
}

void KucoinExecution::keep_private_ws_connection_alive(WebSocketClient& client) {
    std::int64_t now = time_get_now_micro();
    std::string msg = fmt::format(R"({{"id":"{}","op":"ping","timestamp":{}}})", now, now);
    client.start_ping_pong(msg, 15); // 心跳检测时间为20秒
}

Action KucoinExecution::on_ping(Wss* ws, std::string_view payload) {
    // INFRA_LOG_DEBUG("[kucoin] [on_ping] [Execution], payload: {}", payload);
    ws->pong(std::string(payload));
    return Action::NONE;
}

Action KucoinExecution::on_pong(Wss* ws, std::string_view payload) {
    // INFRA_LOG_DEBUG("[kucoin] [on_pong] [Execution], payload: {}", payload);
    return Action::NONE;
}

void KucoinExecution::on_close(Wss* ws) {
    size_t index = ws->get_index();
    INFRA_LOG_WARN("[kucoin] [on_close] [Execution], msg: WebSocket connection has been closed, index:{}", index);
}

void KucoinExecution::on_error(Wss* ws, std::string_view err) {
    size_t index = ws->get_index();
    INFRA_LOG_WARN("[kucoin] [on_error] [Execution], msg: WebSocket error occurred: {}, index:{}", err, index);
    if (index == 1) {
        // kucoin下单连接建立时要求传入时间戳，不能依赖网络库内部的重连机制
        wss_trade_.close();
        auto timer = std::make_shared<boost::asio::steady_timer>(ioc_, std::chrono::seconds(5));
        timer->async_wait([this, timer](const boost::system::error_code& ec) {
            wss_trade_config_.path = get_ws_url(this->account_secret_);
            wss_trade_.resolve_reconnect(wss_trade_config_.host, wss_trade_config_.port, wss_trade_config_.path);
        });
    }
}

Action KucoinExecution::on_message(Wss* ws, std::string_view msg) {
    if (g_account_mode == AccountMode::CLASSIC) {
        return on_classic_message(ws, msg);
    } else if (g_account_mode == AccountMode::UNIFIED) {
        return on_unified_message(ws, msg);
    }
    return Action::RECEIVE;
}

Action KucoinExecution::on_classic_message(Wss* ws, std::string_view msg) {
    size_t index = ws->get_index();
    // INFRA_LOG_INFO("[kucoin] [on_message] [Execution], msg: {}, index: {}", msg, index);
    try {
        PARSE_JSON(msg, doc);
        if (index == 1 && doc["sessionId"].error() == simdjson::SUCCESS &&
            doc["timestamp"].error() == simdjson::SUCCESS) {
            if (!login(msg)) // 对下单连接做签名
            {
                std::string_view err = "login failed";
                on_error(ws, err);
            }
            return Action::RECEIVE;
        }
        if (doc["op"].error() == simdjson::SUCCESS) {
            std::string_view op = doc["op"];
            if (op == "futures.order" || op == "futures.cancel") {
                std::string_view reqId = doc["id"];
                unsigned long uid = std::stoul(std::string(reqId));
                auto iter = ws_request_cache_.find(uid);
                if (iter == ws_request_cache_.end()) {
                    INFRA_LOG_WARN("[kucoin] [on_message] [place_order_ws] [fail], not found callback");
                    return Action::RECEIVE;
                }

                auto [order, cb] = ws_request_cache_[uid];
                if (doc["code"].get_string()->compare(KUCOIN_SUCCESS_CODE) != 0) {
                    INFRA_LOG_WARN("[kucoin] [on_message] [place_order_ws] [fail], msg: {}", msg);
                    order->ec = extract_error_code(msg);
                    order->detail = msg;
                    order->status = OrderStatus::Failed;
                    order->milli = time_get_now_milli();
                    cb(order->ec, order);
                    ws_request_cache_.erase(iter);
                    return Action::RECEIVE;
                }
                if (op == "futures.order") { // place
                    std::string_view orderId = doc["data"]["orderId"];
                    order->market_oid = orderId;
                    order->status = OrderStatus::New;
                    INFRA_LOG_INFO("[kucoin] [place_order_ws] [success], msg: {}", msg);
                } else {
                    order->status = OrderStatus::Canceling;
                    INFRA_LOG_INFO("[kucoin] [cancel_order_ws] [success], msg: {}", msg);
                }
                order->milli = time_get_now_milli();
                cb(Errno::Ok, order);
                ws_request_cache_.erase(iter);
                return Action::RECEIVE;
            }
        }
        if (doc["topic"].error() == simdjson::SUCCESS) {
            std::string_view topic = doc["topic"];
            if (topic == "/contractMarket/tradeOrders") {
                simdjson::dom::object obj = doc["data"];
                SpOrder rtn_order = parse_classic_rtn_order(obj);
                this->dispatch_order(std::move(rtn_order));
                INFRA_LOG_INFO("[kucoin] [on_message] [order], recv: {}", msg);
            } else {
                INFRA_LOG_WARN("[kucoin] [on_message] unexpected msg: {}", msg);
            }
        } else if (doc["type"].error() == simdjson::SUCCESS) {
            std::string_view type = doc["type"];
            if (type == "pong") {
                // ignore
            } else if (type == "welcome" or type == "ack") {
                INFRA_LOG_INFO("[kucoin] [{}] [success], recv: {}", type, msg);
            } else {
                INFRA_LOG_WARN("[kucoin] [on_message], unexpected msg: {}", msg);
            }
        }
    } catch (const std::exception& ex) {
        INFRA_LOG_WARN("[kucoin] [on_message] [Execution], exception error: {}, msg: {}", ex.what(), msg);
    }
    return Action::RECEIVE;
}

Action KucoinExecution::on_unified_message(Wss* ws, std::string_view msg) {
    size_t index = ws->get_index();
    // INFRA_LOG_INFO("[kucoin] [on_message] [Execution], msg: {}, index: {}", msg, index);
    try {
        PARSE_JSON(msg, doc);
        if (index == 1 && doc["sessionId"].error() == simdjson::SUCCESS) {
            if (doc["timestamp"].error() == simdjson::SUCCESS) {
                if (!login(msg)) // 对下单连接做签名
                {
                    std::string_view err = "login failed";
                    on_error(ws, err);
                }
            }
            return Action::RECEIVE;
        }
        if (doc["op"].error() == simdjson::SUCCESS) {
            std::string_view op = doc["op"];
            if (op == "uta.order" || op == "uta.cancel") {
                std::string_view reqId = doc["id"];
                unsigned long uid = std::stoul(std::string(reqId));
                auto iter = ws_request_cache_.find(uid);
                if (iter == ws_request_cache_.end()) {
                    INFRA_LOG_WARN("[kucoin] [on_message] [place_order_ws] [fail], not found callback");
                    return Action::RECEIVE;
                }

                auto [order, cb] = ws_request_cache_[uid];
                if (doc["code"].get_string()->compare(KUCOIN_SUCCESS_CODE) != 0) {
                    INFRA_LOG_WARN("[kucoin] [on_message] [place_order_ws] [fail], msg: {}", msg);
                    order->ec = extract_error_code(msg);
                    order->detail = msg;
                    order->status = OrderStatus::Failed;
                    order->milli = time_get_now_milli();
                    cb(order->ec, order);
                    ws_request_cache_.erase(iter);
                    return Action::RECEIVE;
                }
                if (op == "uta.order") { // place
                    std::string_view orderId = doc["data"]["orderId"];
                    order->market_oid = orderId;
                    order->status = OrderStatus::New;
                    INFRA_LOG_INFO("[kucoin] [place_order_ws] [success], msg: {}", msg);
                } else {
                    order->status = OrderStatus::Canceling;
                    INFRA_LOG_INFO("[kucoin] [cancel_order_ws] [success], msg: {}", msg);
                }
                order->milli = time_get_now_milli();
                cb(Errno::Ok, order);
                ws_request_cache_.erase(iter);
            } else if (op == "pong") {
                // ignore
            }
            return Action::RECEIVE;
        }
        if (doc["T"].error() == simdjson::SUCCESS) {
            std::string_view topic = doc["T"];
            if (topic == "orderAll.UNIFIED") {
                simdjson::dom::object data = doc["d"];
                SpOrder rtn_order = parse_unified_rtn_order(data);
                this->dispatch_order(std::move(rtn_order));
                INFRA_LOG_INFO("[kucoin] [on_message] [order], recv: {}", msg);
            } else {
                INFRA_LOG_WARN("[kucoin] [on_message] unexpected msg: {}", msg);
            }
        } else if (doc["message"].error() == simdjson::SUCCESS) {
            std::string_view message = doc["message"];
            if (message == "welcome") {
                INFRA_LOG_INFO("[kucoin] [{}] [success], recv: {}", message, msg);
            } else {
                INFRA_LOG_WARN("[kucoin] [on_message], unexpected msg: {}", msg);
            }
        } else if (doc["type"].error() == simdjson::SUCCESS) {
            std::string_view type = doc["type"];
            if (type == "pong") {
                // ignore
            } else {
                INFRA_LOG_WARN("[kucoin] [on_message], unexpected msg: {}", msg);
            }
        } else if (doc["result"].error() == simdjson::SUCCESS) {
            bool result = doc["result"];
            if (result) {
                // pass
            } else {
                INFRA_LOG_WARN("[kucoin] [on_message], unexpected msg: {}", msg);
            }
        } else {
            INFRA_LOG_WARN("[kucoin] [on_message], unexpected msg: {}", msg);
        }
    } catch (const std::exception& ex) {
        INFRA_LOG_WARN("[kucoin] [on_message] [Execution], exception error: {}, msg: {}", ex.what(), msg);
    }
    return Action::RECEIVE;
}

bool KucoinExecution::login(std::string_view msg) {
    std::string payload = generate_sign_hmac256_b64(account_secret_.api_secret, std::string(msg));
    return send_ws_request(wss_trade_, payload, "login_api");
}

bool KucoinExecution::send_ws_request(WebSocketClient& client, const std::string& content, const std::string& name) {
    if (client.is_socket_open()) {
        client.send(content);
        INFRA_LOG_INFO("[kucoin] [{}], send: {}", name, content);
        return true;
    } else {
        INFRA_LOG_WARN("[kucoin] [{}] [fail], msg: WebSocket not connected", name);
        return false;
    }
}

bool KucoinExecution::convert_place_classic_order(const SpOrder order, const OrderCallback& cb, std::string& payload) {
    if (order->type != OrderType::Limit && order->type != OrderType::Market) {
        INFRA_LOG_WARN("[kucoin] [convert_place_classic_order] [fail], msg: order type is not supported");
        cb(Errno::InvalidParams, order);
        return false;
    }

    if (order->client_oid.empty() || order->pair.empty()) {
        INFRA_LOG_WARN("[kucoin] [convert_place_classic_order] [fail], msg: client_oid or pair is empty");
        cb(Errno::InvalidParams, order);
        return false;
    }

    auto it = g_pairs_info_cache.find(order->pair);
    if (it == g_pairs_info_cache.end()) {
        INFRA_LOG_WARN("[kucoin] [convert_place_classic_order] [fail], msg: not found {} in cache", order->pair);
        cb(Errno::InvalidParams, order);
        return false;
    }

    SpExPairInfo pair_info = it->second;

    std::string side;
    std::string position_side;
    bool reduce_only = false;

    if (g_current_position_mode == PositionMode::one_way_mode) {
        position_side = "BOTH";
        if (order->side == OrderSide::OpenLong || order->side == OrderSide::CloseShort) {
            side = "buy";
        } else {
            side = "sell";
        }

        if (order->side == OrderSide::CloseLong || order->side == OrderSide::CloseShort) {
            reduce_only = true;
        }
    } else {
        if (order->side == OrderSide::OpenLong) {
            side = "buy";
            position_side = "LONG";
        } else if (order->side == OrderSide::CloseLong) {
            side = "sell";
            position_side = "LONG";
        } else if (order->side == OrderSide::OpenShort) {
            side = "sell";
            position_side = "SHORT";
        } else if (order->side == OrderSide::CloseShort) {
            side = "buy";
            position_side = "SHORT";
        }
    }

    bool post_only{false};
    std::string tif_str{"GTC"};
    if (order->tif == OrderTIF::IOC) {
        tif_str = "IOC";
    } else if (order->tif == OrderTIF::MAKER) {
        post_only = true;
    }

    std::string margin_mode{};
    std::string dynamic_parts;
    MarginMode current_margin_mode{g_default_margin_mode};
    if (g_current_symbol_margin_mode.count(order->pair)) {
        current_margin_mode = g_current_symbol_margin_mode[order->pair];
    }

    if (current_margin_mode == MarginMode::CROSS) {
        margin_mode = "CROSS";
        if (order->side == OrderSide::OpenLong or order->side == OrderSide::OpenShort or
            order->side == OrderSide::OpenOnly) {
            dynamic_parts += fmt::format(R"("leverage":{},)", order->par_leverage);
        }
    } else {
        margin_mode = "ISOLATED";
        dynamic_parts += fmt::format(R"("leverage":{},)", order->par_leverage);
    }

    double size = static_cast<int>(order->quantity / pair_info->denomination_value); // 币数转张数
    double price = order->price;
    price = int(price / pair_info->step_size_quote) * pair_info->step_size_quote;

    std::string type_str = (order->type == OrderType::Market) ? "market" : "limit";

    payload = fmt::format(
        R"({{"symbol":"{}","side":"{}","clientOid":"{}","type":"{}","size":"{}","price":"{}","reduceOnly":{},"timeInForce":"{}",{}"positionSide":"{}","marginMode":"{}","postOnly":{}}})",
        transfer_from_infra_pair(order->pair), // symbol
        side,                                  // side
        order->client_oid,                     // clientOid
        type_str,                              // type
        float_to_compact_str(size),            // size
        float_to_compact_str(price),           // price
        reduce_only,                           // reduceOnly
        tif_str,                               // timeInForce
        dynamic_parts,                         // dynamic_parts
        position_side,                         // positionSide
        margin_mode,                           // marginMode
        post_only                              // postOnly
    );

    return true;
}
bool KucoinExecution::convert_place_unified_order(const SpOrder order, const OrderCallback& cb, std::string& payload) {
    if (order->type != OrderType::Limit && order->type != OrderType::Market) {
        INFRA_LOG_WARN("[kucoin] [convert_place_unified_order] [fail], msg: order type is not supported");
        cb(Errno::InvalidParams, order);
        return false;
    }

    if (order->client_oid.empty() || order->pair.empty()) {
        INFRA_LOG_WARN("[kucoin] [convert_place_unified_order] [fail], msg: client_oid or pair is empty");
        cb(Errno::InvalidParams, order);
        return false;
    }

    auto it = g_pairs_info_cache.find(order->pair);
    if (it == g_pairs_info_cache.end()) {
        INFRA_LOG_WARN("[kucoin] [convert_place_unified_order] [fail], msg: not found {} in cache", order->pair);
        cb(Errno::InvalidParams, order);
        return false;
    }

    SpExPairInfo pair_info = it->second;

    std::string side;
    bool reduce_only = false;

    if (g_current_position_mode == PositionMode::one_way_mode) {
        if (order->side == OrderSide::OpenLong || order->side == OrderSide::CloseShort) {
            side = "BUY";
        } else {
            side = "SELL";
        }

        if (order->side == OrderSide::CloseLong || order->side == OrderSide::CloseShort) {
            reduce_only = true;
        }
    } else {
        if (order->side == OrderSide::OpenLong) {
            side = "BUY";
        } else if (order->side == OrderSide::CloseLong) {
            side = "SELL";
        } else if (order->side == OrderSide::OpenShort) {
            side = "SELL";
        } else if (order->side == OrderSide::CloseShort) {
            side = "BUY";
        }
    }

    bool post_only{false};
    std::string tif_str{"GTC"};
    if (order->tif == OrderTIF::IOC) {
        tif_str = "IOC";
    } else if (order->tif == OrderTIF::FOK) {
        tif_str = "FOK";
    } else if (order->tif == OrderTIF::MAKER) {
        post_only = true;
    }

    double size = static_cast<int>(order->quantity / pair_info->denomination_value); // 币数转张数
    double price = order->price;
    price = int(price / pair_info->step_size_quote) * pair_info->step_size_quote;

    std::string type_str{};
    std::string dynamic_parts{};
    if (order->type == OrderType::Market) {
        type_str = "MARKET";
    } else {
        type_str = "LIMIT";
        dynamic_parts += fmt::format(R"("price":"{}",)", float_to_compact_str(price));
    }

    payload = fmt::format(
        R"({{"tradeType":"FUTURES","sizeUnit":"UNIT","symbol":"{}","side":"{}","clientOid":"{}","orderType":"{}","size":"{}",{}"reduceOnly":{},"timeInForce":"{}","postOnly":{}}})",
        transfer_from_infra_pair(order->pair), // symbol
        side,                                  // side
        order->client_oid,                     // clientOid
        type_str,                              // orderType
        float_to_compact_str(size),            // size
        dynamic_parts,                         // dynamic_parts
        reduce_only,                           // reduceOnly
        tif_str,                               // timeInForce
        post_only                              // postOnly
    );

    return true;
}

void KucoinExecution::send_http_request(const HttpRequestBody& req, SpOrder order, OrderCallback cb,
                                        std::string_view name) {
    rest_.send(req, [this, order, cb, name](HttpResponseBody& res) {
        std::string response = boost::beast::buffers_to_string(res.body().data());
        do {
            if (res.result() != HTTP_STATUS_OK) {
                break;
            }
            try {
                PARSE_JSON(response, doc);
                if (doc["code"].error() != simdjson::SUCCESS ||
                    doc["code"].get_string()->compare(KUCOIN_SUCCESS_CODE) != 0) {
                    break;
                }
                if (name == "place_order_rest") {
                    std::string_view orderId = doc["data"]["orderId"];
                    order->market_oid = orderId;
                    order->status = OrderStatus::New;
                } else if (name == "cancel_order_rest") {
                    order->status = OrderStatus::Canceling;
                } else if (name == "query_order") {
                    simdjson::dom::object obj = doc["data"];
                    if (g_account_mode == AccountMode::CLASSIC) {
                        SpOrder query_order = parse_classic_query_order(obj);
                        order->update(*query_order);
                    } else if (g_account_mode == AccountMode::UNIFIED) {
                        SpOrder query_order = parse_unified_query_order(obj);
                        order->update(*query_order);
                    }
                }

                INFRA_LOG_INFO("[kucoin] [{}] [success], recv: {}", name, response);
                order->milli = time_get_now_milli();
                cb(Errno::Ok, order);
                return;
            } catch (const std::exception& ex) {
                INFRA_LOG_WARN("[kucoin] [{}], exception: {}", name, ex.what());
            }
        } while (0);
        INFRA_LOG_WARN("[kucoin] [{}] [fail], recv: {}", name, response);
        order->ec = extract_error_code(response);
        order->detail = response;
        order->status = OrderStatus::Failed;
        order->milli = time_get_now_milli();
        cb(order->ec, order);
    });
}

void KucoinExecution::get_account_mode() {
    auto req = get_request_body_with_sign(HTTP_GET, "api.kucoin.com", "/api/ua/v1/account/mode", "", account_secret_);
    boost::beast::error_code ec;
    std::string response = rest_.sync_send(req, ec);
    do {
        if (ec) {
            break;
        }
        try {
            PARSE_JSON(response, doc);
            if (doc["code"].get_string()->compare(KUCOIN_SUCCESS_CODE) != 0) {
                break;
            }
            std::string_view selfAccountMode = doc["data"]["selfAccountMode"];
            if (selfAccountMode == "CLASSIC") {
                g_account_mode = AccountMode::CLASSIC;
                INFRA_LOG_INFO("[kucoin] [get_account_mode], use classic mode");
            }
            INFRA_LOG_INFO("[kucoin] [get_account_mode] [success], recv: {}", response);
            return;
        } catch (const std::exception& ex) {
            INFRA_LOG_WARN("[kucoin] [get_account_mode] [exception], msg: {}", ex.what());
        }
    } while (0);
    INFRA_LOG_WARN("[kucoin] [get_account_mode] [fail], recv: {}", response);
}
} // namespace infra