#include "aster_execution.h"
#include <boost/asio/steady_timer.hpp>
using namespace infra::aster;

namespace infra {
bool AsterExecution::initialize() {
    auto& info = g_config_map[base_config_.to_str()];
    if (info.empty()) {
        INFRA_LOG_WARN("[aster] [initialize] [fail], msg: {} {} {} not implemented",
                       to_string(base_config_.account_type), to_string(base_config_.address_type),
                       to_string(base_config_.settle_unit));
        return false;
    }

    if (account_secret_.api_secret.empty() || account_secret_.wallet_address.empty()) {
        INFRA_LOG_WARN("[aster] [initialize] [fail], msg: AccountSecret filed is empty");
        return false;
    }

    if (account_secret_.custom_info.count("signer") == 0 || account_secret_.custom_info["signer"].empty()) {
        INFRA_LOG_WARN("[aster] [initialize] [fail], msg: AccountSecret filed: custom_info[account_id] is empty");
        return false;
    }

    rest_host_ = info[REST_HOST];
    order_path_ = info[ORDER_PATH_PATH];
    listen_key_path_ = info[LISTEN_KEY_PATH];
    wss_config_ = {info[WSS_PRIVATE_HOST], info[WSS_PORT], info[WSS_PRIVATE_PATH]};
    login();
    INFRA_LOG_INFO("[aster] [initialize] [Execution], websocket endpoint: {} {} {}", wss_config_.host, wss_config_.path,
                   wss_config_.port);
    return true;
}

void AsterExecution::shutdown() { unsubscribe_order(); }

void AsterExecution::query_order(const SpOrder order, OrderCallback cb) {
    if (order->market_oid.empty() || order->pair.empty()) {
        INFRA_LOG_WARN("[aster] [query_order] [fail], msg: market_oid or pair is empty");
        cb(Errno::InvalidParams, order);
        return;
    }

    std::string query{};
    query.append("orderId=").append(order->market_oid);
    query.append("&symbol=").append(transfer_from_infra_pair(order->pair));
    query.append("&timestamp=").append(std::to_string(time_get_now_milli()));
    auto req = get_request_body_with_sign(HTTP_GET, rest_host_, order_path_, query, account_secret_);
    send_http_request(req, order, cb, "query_order");
    INFRA_LOG_INFO("[aster] [query_order], send: {}", query);
}

void AsterExecution::place_order_rest(const SpOrder order, OrderCallback cb) {
    std::string query{};
    if (!convert_place_order(order, cb, query)) {
        return;
    }

    auto req = get_request_body_with_sign(HTTP_POST, rest_host_, order_path_, query, account_secret_);
    send_http_request(req, order, cb, "place_order_rest");
    this->add_order_cache(order);
    INFRA_LOG_INFO("[aster] [place_order_rest], send: {}", query);
}

void AsterExecution::cancel_order_rest(const SpOrder order, OrderCallback cb) {
    if (order->market_oid.empty()) {
        INFRA_LOG_WARN("[aster] [cancel_order_rest] [fail], msg: market_oid is empty");
        cb(Errno::InvalidParams, order);
        return;
    }

    std::string payload{};
    payload.append("orderId=").append(order->market_oid);
    payload.append("&symbol=").append(transfer_from_infra_pair(order->pair));
    payload.append("&timestamp=").append(std::to_string(time_get_now_milli()));
    auto req = get_request_body_with_sign(HTTP_DELETE, rest_host_, order_path_, payload, account_secret_);
    send_http_request(req, order, cb, "cancel_order_rest");
    INFRA_LOG_INFO("[aster] [cancel_order_rest], send: {}", payload);
}

bool AsterExecution::subscribe_order(OrderCallback cb) {
    if (listen_key_.empty()) {
        get_listen_key();
        INFRA_LOG_WARN("[aster] [subscribe_order] [fail], msg: listen key is empty, please try again later");
        return false;
    }

    this->order_handler_ = std::move(cb);
    // NOTE：resolve_connect要求传string_view参数，使用tmp_path来保证变量生命周期
    tmp_path = wss_config_.path + listen_key_;
    wss_stream_.resolve_connect(wss_config_.host, wss_config_.port, tmp_path);
    wss_stream_.set_user_data(1);
    return true;
}

void AsterExecution::unsubscribe_order() {
    listen_key_.clear();
    if (LIKELY(wss_stream_.is_socket_open())) {
        wss_stream_.close();
    }
    this->order_handler_ = nullptr;
    INFRA_LOG_INFO("[aster] [unsubscribe_order] [success]");
}

void AsterExecution::place_order_ws(const SpOrder order, OrderCallback cb) { place_order_rest(order, cb); }

void AsterExecution::cancel_order_ws(const SpOrder order, OrderCallback cb) { cancel_order_rest(order, cb); }

Action AsterExecution::on_connect(Wss* ws) {
    INFRA_LOG_INFO("[aster] [on_connect] [Execution], msg: WebSocket connection established");
    // keep_ws_connection_alive(wss_stream_);
    return Action::NONE;
}

Action AsterExecution::on_ping(Wss* ws, std::string_view payload) {
    // INFRA_LOG_DEBUG("[aster] [on_ping] [Execution], payload: {}", payload);
    ws->pong(std::string(payload));
    return Action::NONE;
}

Action AsterExecution::on_pong(Wss* ws, std::string_view payload) {
    // INFRA_LOG_DEBUG("[aster] [on_pong] [Execution], payload: {}", payload);
    return Action::NONE;
}

void AsterExecution::on_close(Wss* ws) {
    INFRA_LOG_WARN("[aster] [on_close] [Execution], msg: WebSocket connection has been closed");
}

void AsterExecution::on_error(Wss* ws, std::string_view err) {
    INFRA_LOG_WARN("[aster] [on_error] [Execution], msg: WebSocket error occurred: {}", err);
}

Action AsterExecution::on_message(Wss* ws, std::string_view msg) {
    INFRA_LOG_INFO("[aster] [on_message] [Execution], msg: {}", msg);
    try {
        PARSE_JSON(msg, doc);
        if (doc["e"].error() == simdjson::SUCCESS) {
            std::string_view event = doc["e"];
            if (event == "ORDER_TRADE_UPDATE") {
                simdjson::dom::object data = doc["o"];
                SpOrder rtn_order = parse_rtn_order(data, false);
                this->process_rtn_order(std::move(rtn_order));
                INFRA_LOG_INFO("[aster] [on_message] [order], recv: {}", msg);
            } else if (event == "listenKeyExpired") {
                INFRA_LOG_WARN("[aster] [on_message] [fail], msg: listen key has expired");
                get_listen_key(true);
            } else {
                // NOTE：忽略其他事件
            }
        } else if (doc["id"].error() == simdjson::SUCCESS) {
            INFRA_LOG_INFO("[aster] [on_message], msg: {}", msg);
            bool response_ok = false;
            if (doc["result"].error() == simdjson::SUCCESS && doc["error"].error() != simdjson::SUCCESS) {
                response_ok = true;
            }

            std::string_view type = doc["id"];
            if (type.find("stream") != std::string_view::npos) {
                if (response_ok) {
                    std::string_view key = doc["result"]["listenKey"];
                    if (key != listen_key_) {
                        INFRA_LOG_WARN(
                            "[aster] [on_message] [fail], msg: listen key mismatch, received: {}, expected: {}", key,
                            listen_key_);
                        get_listen_key(true);
                    }
                }
            }
        }
    } catch (const std::exception& ex) {
        INFRA_LOG_WARN("[aster] [on_message] [exception], error: {}, msg: {}", ex.what(), msg);
    }
    return Action::RECEIVE;
}

void AsterExecution::login() {
    get_listen_key();
    keep_listen_key();
}

void AsterExecution::get_listen_key(bool subscribed) {
    auto req = get_request_body_with_sign(HTTP_POST, rest_host_, listen_key_path_, "", account_secret_);
    send_http_request(
        req, nullptr,
        [this, subscribed](Errno ec, SpOrder /*order*/) {
            if (ec != Errno::Ok) {
                INFRA_LOG_WARN("[aster] [get_listen_key] [fail], msg: failed to get listen key");
                if (subscribed) {
                    get_listen_key(true); // retry
                }
                return;
            }
            INFRA_LOG_INFO("[aster] [get_listen_key] [success], msg: listen key obtained: {}", listen_key_);
            if (subscribed) {
                this->unsubscribe_order();
                OrderCallback cb = order_handler_;
                this->subscribe_order(cb);
                return;
            }
        },
        "get_listen_key");
}

void AsterExecution::keep_listen_key() {
    auto timer = std::make_shared<net::steady_timer>(ioc_, std::chrono::seconds(45 * 60));
    timer->async_wait([this, timer](const boost::system::error_code& ec) {
        if (ec) {
            keep_listen_key();
            return;
        }
        auto req = get_request_body_with_sign(HTTP_PUT, rest_host_, listen_key_path_, "", account_secret_);
        send_http_request(
            req, nullptr,
            [this](Errno ec, SpOrder /*order*/) {
                if (ec != Errno::Ok) {
                    INFRA_LOG_WARN("[aster] [keep_listen_key] [fail], msg: failed to keep listen key alive");
                    return;
                }
                INFRA_LOG_INFO("[aster] [keep_listen_key] [success], msg: listen key kept alive");
            },
            "keep_listen_key");
        keep_listen_key();
    });
}

bool AsterExecution::convert_place_order(SpOrder order, OrderCallback cb, std::string& payload) {
    if (order->client_oid.empty() || order->pair.empty()) {
        INFRA_LOG_WARN("[aster] [convert_place_order] [fail], msg: client_oid or pair is empty");
        cb(Errno::InvalidParams, order);
        return false;
    }

    auto it = g_pairs_info_cache.find(to_lower_str(order->pair));
    if (it == g_pairs_info_cache.end()) {
        INFRA_LOG_WARN("[aster] [convert_place_order] [fail], msg: not found {} in cache", order->pair);
        cb(Errno::InvalidParams, order);
        return false;
    }

    SpExPairInfo pair_info = it->second;
    std::map<std::string, std::string> params;
    params["newClientOrderId"] = order->client_oid;
    params["symbol"] = transfer_from_infra_pair(order->pair);
    params["timestamp"] = std::to_string(time_get_now_milli());
    params["recvWindow"] = "50000";
    if (order->side == OrderSide::OpenLong) {
        params["side"] = "BUY";
        params["positionSide"] = "LONG";
    } else if (order->side == OrderSide::OpenShort) {
        params["side"] = "SELL";
        params["positionSide"] = "SHORT";
    } else if (order->side == OrderSide::CloseLong) {
        params["side"] = "SELL";
        params["positionSide"] = "LONG";
        if (g_current_position_mode == PositionMode::one_way_mode) {
            params["reduceOnly"] = "true";
        }
    } else if (order->side == OrderSide::CloseShort) {
        params["side"] = "BUY";
        params["positionSide"] = "SHORT";
        if (g_current_position_mode == PositionMode::one_way_mode) {
            params["reduceOnly"] = "true";
        }
    }

    // NOTE：单向持仓模式下，positionSide参数固定为BOTH
    if (g_current_position_mode == PositionMode::one_way_mode) {
        params["positionSide"] = "BOTH";
    }
    bfloat price = order->price;
    switch (order->type) {
        case OrderType::Limit:
            params["type"] = "LIMIT";
            price = int(price / pair_info->step_size_quote) * pair_info->step_size_quote;
            params["price"] = float_to_compact_str(price);
            params["timeInForce"] = to_string(order->tif);
            break;
        case OrderType::Market:
            params["type"] = "MARKET";
            break;
        default:
            INFRA_LOG_WARN("[aster] [convert_place_order] [fail], msg: invalid order type: {}", to_string(order->type));
            cb(Errno::InvalidParams, order);
            return false;
    }

    bfloat quantity = order->quantity;
    quantity = int(quantity / pair_info->step_size_base) * pair_info->step_size_base;
    params["quantity"] = float_to_compact_str(quantity);
    payload = map_to_query_str(params);
    return true;
}

void AsterExecution::send_http_request(const HttpRequestBody& req, SpOrder order, OrderCallback cb,
                                       std::string_view name) {
    rest_.send(req, [this, order, cb, name](HttpResponseBody& res) {
        std::string response = boost::beast::buffers_to_string(res.body().data());
        do {
            if (res.result() != HTTP_STATUS_OK) {
                break;
            }
            try {
                PARSE_JSON(response, doc);
                if (name == "keep_listen_key") {
                    cb(Errno::Ok, nullptr);
                    return;
                }
                if (doc["code"].error() == simdjson::SUCCESS && doc["code"].get_int64() != SUCCESS_CODE) {
                    break;
                }
                if (name != "get_listen_key") {
                    SpOrder rtn_order = parse_rtn_order(doc, true);
                    order->update(*rtn_order);
                } else {
                    std::string_view result_text = doc["listenKey"];
                    this->listen_key_ = result_text;
                    INFRA_LOG_INFO("[aster] [get_listen_key] [success], msg: {}", result_text);
                    cb(Errno::Ok, nullptr);
                    return;
                }
                INFRA_LOG_INFO("[aster] [{}] [success], recv: {}", name, response);
                order->milli = time_get_now_milli();
                cb(Errno::Ok, order);
                return;
            } catch (const std::exception& ex) {
                INFRA_LOG_WARN("[aster] [{}], exception: {}", name, ex.what());
            }
        } while (0);
        INFRA_LOG_WARN("[aster] [{}] [fail], recv: {}", name, response);
        if (name == "get_listen_key" || name == "keep_listen_key") {
            cb(Errno::ApiError, nullptr);
            return;
        }
        order->ec = extract_error_code(response);
        order->detail = response;
        order->status = OrderStatus::Failed;
        order->milli = time_get_now_milli();
        cb(order->ec, order);
    });
}
} // namespace infra