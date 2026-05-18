#include "toobit_execution.h"
using namespace infra::toobit;

namespace infra {
bool ToobitExecution::initialize() {
    auto& info = g_config_map[base_config_.to_str()];
    if (info.empty()) {
        INFRA_LOG_WARN("[toobit] [initialize] [fail], msg: {} {} {} not implemented",
                       to_string(base_config_.account_type), to_string(base_config_.address_type),
                       to_string(base_config_.settle_unit));
        return false;
    }

    if (account_secret_.api_key.empty() || account_secret_.api_secret.empty()) {
        INFRA_LOG_WARN("[toobit] [initialize] [fail], msg: AccountSecret filed is empty");
        return false;
    }

    rest_host_ = info[REST_HOST];
    order_path_ = info[ORDER_PATH_PATH];
    listen_key_path_ = info[LISTEN_KEY_PATH];

    wss_config_ = {info[WSS_PRIVATE_HOST], info[WSS_PORT], info[WSS_PRIVATE_PATH]};
    INFRA_LOG_INFO("[toobit] [initialize] [Execution], websocket endpoint: {} {} {}", wss_config_.host,
                   wss_config_.path, wss_config_.port);
    get_listen_key();
    return true;
}

void ToobitExecution::shutdown() { unsubscribe_order(); }

void ToobitExecution::query_order(const SpOrder order, OrderCallback cb) {
    if (order->market_oid.empty()) {
        INFRA_LOG_WARN("[toobit] [query_order] [fail], msg: market_oid is empty");
        cb(Errno::InvalidParams, order);
        return;
    }

    std::string query{};
    query.append("orderId=").append(order->market_oid);
    query.append("&timestamp=").append(std::to_string(time_get_now_milli()));
    auto req = get_request_body_with_sign(HTTP_GET, rest_host_, order_path_, query, account_secret_);
    send_http_request(req, order, cb, "query_order");
    INFRA_LOG_INFO("[toobit] [query_order], send: {}", query);
}

void ToobitExecution::place_order_rest(const SpOrder order, OrderCallback cb) {
    std::string payload{};
    if (!convert_place_order(order, cb, payload)) {
        return;
    }

    auto req = get_request_body_with_sign(HTTP_POST, rest_host_, order_path_, payload, account_secret_);
    send_http_request(req, order, cb, "place_order_rest");
    this->add_order_cache(order);
    INFRA_LOG_INFO("[toobit] [place_order_rest], send: {}", payload);
}

void ToobitExecution::cancel_order_rest(const SpOrder order, OrderCallback cb) {
    INFRA_LOG_INFO("[toobit] [cancel_order_rest] [fail], not supported");
    order->ec = Errno::NotSupported;
    order->detail = "not supported";
    order->status = OrderStatus::Failed;
    order->milli = time_get_now_milli();
    cb(order->ec, order);
    return; // NOTE: 特殊处理, 直接返回错误，不报出去
    
    if (order->market_oid.empty()) {
        INFRA_LOG_WARN("[toobit] [cancel_order_rest] [fail], msg: market_oid is empty");
        cb(Errno::InvalidParams, order);
        return;
    }

    std::string query{};
    query.append("orderId=").append(order->market_oid);
    query.append("&timestamp=").append(std::to_string(time_get_now_milli()));
    auto req = get_request_body_with_sign(HTTP_DELETE, rest_host_, order_path_, query, account_secret_);
    send_http_request(req, order, cb, "cancel_order_rest");
    INFRA_LOG_INFO("[toobit] [cancel_order_rest], send: {}", query);
}

bool ToobitExecution::subscribe_order(OrderCallback cb) {
    if (listen_key_.empty() && !get_listen_key_sync()) {
        INFRA_LOG_WARN("[toobit] [subscribe_order] [fail], unable to get listenKey");
        return false;
    }

    keep_listen_key_alive();
    this->order_handler_ = std::move(cb);
    static std::string real_ws_path{}; // 使用static保证string_view生命周期
    real_ws_path = wss_config_.path + listen_key_;
    wss_stream_.resolve_connect(wss_config_.host, wss_config_.port, real_ws_path);
    return true;
}

void ToobitExecution::unsubscribe_order() {
    wss_stream_.close();
    listen_key_.clear();
    this->order_handler_ = nullptr;
    INFRA_LOG_INFO("[toobit] [unsubscribe_order] [success]");
}

void ToobitExecution::place_order_ws(const SpOrder order, OrderCallback cb) { place_order_rest(order, cb); }

void ToobitExecution::cancel_order_ws(const SpOrder order, OrderCallback cb) { cancel_order_rest(order, cb); }

Action ToobitExecution::on_connect(Wss* ws) {
    INFRA_LOG_INFO("[toobit] [on_connect] [Execution], msg: WebSocket connection established");
    // keep_ws_connection_alive();
    return Action::NONE;
}

Action ToobitExecution::on_ping(Wss* ws, std::string_view payload) {
    // INFRA_LOG_DEBUG("[toobit] [on_ping] [Execution], payload: {}", payload);
    ws->pong(std::string(payload));
    return Action::NONE;
}

Action ToobitExecution::on_pong(Wss* ws, std::string_view payload) {
    // INFRA_LOG_DEBUG("[toobit] [on_pong] [Execution], payload: {}", payload);
    return Action::NONE;
}

void ToobitExecution::on_close(Wss* ws) {
    INFRA_LOG_WARN("[toobit] [on_close] [Execution], msg: WebSocket connection has been closed");
}

void ToobitExecution::on_error(Wss* ws, std::string_view err) {
    INFRA_LOG_WARN("[toobit] [on_error] [Execution], msg: WebSocket error occurred: {}", err);
}

Action ToobitExecution::on_message(Wss* ws, std::string_view msg) {
    // INFRA_LOG_INFO("[toobit] [on_message] [Execution], msg: {}", msg);
    try {
        PARSE_JSON(msg, doc);
        if (doc.is_array()) {
            simdjson::dom::array array = doc.get_array();
            for (auto item : array) {
                std::string_view event = item["e"];
                if (event == "contractExecutionReport") {
                    INFRA_LOG_INFO("[toobit] [on_message] [order], msg: {}", msg);
                    auto rtn_order = parse_rtn_order(item, false);
                    this->process_rtn_order(std::move(rtn_order));
                } else {
                    // 忽略其他事件
                }
            }
        } else if (doc["ping"].error() == simdjson::SUCCESS) {
            // ignore
        } else {
            INFRA_LOG_INFO("[bingx] [on_message], unexcepted msg: {}", msg);
        }
    } catch (const std::exception& ex) {
        INFRA_LOG_WARN("[toobit] [on_message] [exception], error: {}, msg: {}", ex.what(), msg);
    }
    return Action::RECEIVE;
}

void ToobitExecution::keep_ws_connection_alive() {
    std::string msg = fmt::format(R"({{"ping":{}}})", time_get_now_milli());
    wss_stream_.start_ping_pong(msg, 10);
}

void ToobitExecution::get_listen_key() {
    std::string query{};
    query.append("timestamp=").append(std::to_string(time_get_now_milli()));
    auto req = get_request_body_with_sign(HTTP_POST, rest_host_, listen_key_path_, query, account_secret_);
    rest_.send(req, [this](HttpResponseBody& res) {
        std::string response = boost::beast::buffers_to_string(res.body().data());
        do {
            if (res.result() != HTTP_STATUS_OK) {
                break;
            }
            try {
                PARSE_JSON(response, doc);
                std::string_view listenKey = doc["listenKey"];
                this->listen_key_ = listenKey;
                INFRA_LOG_INFO("[toobit] [get_listen_key] [success], recv: {}", response);
                return;
            } catch (const std::exception& ex) {
                INFRA_LOG_WARN("[toobit] [get_listen_key] [exception], msg: {}", ex.what());
            }
        } while (0);
        INFRA_LOG_WARN("[toobit] [get_listen_key] [fail], recv: {}", response);
    });
}

bool ToobitExecution::get_listen_key_sync() {
    std::string query{};
    query.append("timestamp=").append(std::to_string(time_get_now_milli()));
    auto req = get_request_body_with_sign(HTTP_POST, rest_host_, listen_key_path_, query, account_secret_);
    boost::beast::error_code ec;
    std::string response = rest_.sync_send(req, ec);
    do {
        if (ec) {
            break;
        }
        try {
            PARSE_JSON(response, doc);
            std::string_view listenKey = doc["listenKey"];
            this->listen_key_ = listenKey;
            INFRA_LOG_INFO("[toobit] [get_listen_key_sync] [success], recv: {}", response);
            return true;
        } catch (const std::exception& ex) {
            INFRA_LOG_WARN("[toobit] [get_listen_key_sync] [exception], msg: {}", ex.what());
        }
    } while (0);
    INFRA_LOG_WARN("[toobit] [get_listen_key_sync] [fail], recv: {}", response);
    return false;
}

void ToobitExecution::keep_listen_key_alive() {
    auto timer = std::make_shared<boost::asio::steady_timer>(ioc_, std::chrono::minutes(25));
    timer->async_wait([this, timer](const boost::system::error_code& ec) {
        if (ec) {
            INFRA_LOG_WARN("[toobit] [keep_listen_key_alive] steady_timer error occurred: {}", ec.message());
        }
        std::string query = "listenKey=" + listen_key_;
        INFRA_LOG_INFO("[toobit] [keep_listen_key_alive] send: {}", query);
        auto req = get_request_body_with_sign(HTTP_PUT, rest_host_, listen_key_path_, query, account_secret_);
        rest_.send(req, [this](HttpResponseBody& res) {
            // NOTE：响应为空
            if (res.result() != HTTP_STATUS_OK) {
                INFRA_LOG_WARN("[toobit] [keep_listen_key_alive] [fail], recv: {}");
            }
        });
        keep_listen_key_alive();
    });
}

bool ToobitExecution::convert_place_order(SpOrder order, OrderCallback cb, std::string& payload) {
    if (order->type != OrderType::Limit && order->type != OrderType::Market) {
        INFRA_LOG_WARN("[toobit] [convert_place_order] [fail], msg: order type is not supported");
        cb(Errno::InvalidParams, order);
        return false;
    }

    if (order->client_oid.empty() || order->pair.empty()) {
        INFRA_LOG_WARN("[toobit] [convert_place_order] [fail], msg: client_oid or pair is empty");
        cb(Errno::InvalidParams, order);
        return false;
    }

    auto it = g_pairs_info_cache.find(to_lower_str(order->pair));
    if (it == g_pairs_info_cache.end()) {
        INFRA_LOG_WARN("[toobit] [place_ioc_order] [fail], msg: not found {} in cache", order->pair);
        cb(Errno::InvalidParams, order);
        return false;
    }

    SpExPairInfo pair_info = it->second;
    if (order->quantity < pair_info->denomination_value) {
        INFRA_LOG_WARN(
            "[toobit] [convert_place_order] [fail], msg: order quantity {} is lesser than denomination value {}",
            order->quantity.str(), pair_info->denomination_value.str());
        order->ec = Errno::SmallSizeOrder;
        cb(order->ec, order);
        return false;
    }

    int size = static_cast<int>(order->quantity / pair_info->denomination_value);               // 币数转张数
    bfloat price = int(order->price / pair_info->step_size_quote) * pair_info->step_size_quote; // 调整价格精度

    std::string_view priceType = (order->type == OrderType::Limit) ? "INPUT" : "MARKET";
    std::string tifStr{};
    if (order->tif == OrderTIF::GTC) {
        tifStr = "GTC";
    } else if (order->tif == OrderTIF::IOC) {
        tifStr = "IOC";
    } else if (order->tif == OrderTIF::FOK) {
        tifStr = "FOK";
    } else {
        INFRA_LOG_WARN("[toobit] [convert_place_order] [fail], msg: order tif is not supported");
        cb(Errno::InvalidParams, order);
        return false;
    }

    std::string side{};
    if (order->side == OrderSide::OpenLong) {
        side = "BUY_OPEN";
    } else if (order->side == OrderSide::OpenShort) {
        side = "SELL_OPEN";
    } else if (order->side == OrderSide::CloseLong) {
        side = "SELL_CLOSE";
    } else if (order->side == OrderSide::CloseShort) {
        side = "BUY_CLOSE";
    }

    payload = fmt::format("symbol={}&side={}&type=LIMIT&quantity={}&price={}&priceType={}&"
                          "timeInForce={}&newClientOrderId={}&timestamp={}",
                          transfer_from_infra_pair(order->pair), side, size, float_to_compact_str(price), priceType,
                          tifStr, order->client_oid, time_get_now_milli());
    return true;
}

void ToobitExecution::send_http_request(const HttpRequestBody& req, SpOrder order, OrderCallback cb,
                                        std::string_view name) {
    rest_.send(req, [this, order, cb, name](HttpResponseBody& res) {
        std::string response = boost::beast::buffers_to_string(res.body().data());
        do {
            if (res.result() != HTTP_STATUS_OK) {
                break;
            }
            try {
                PARSE_JSON(response, doc);
                if (doc["code"].error() == simdjson::SUCCESS) {
                    break; // 有code字段，说明是错误响应
                }
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
                INFRA_LOG_INFO("[toobit] [{}] [success], recv: {}", name, response);
                order->milli = time_get_now_milli();
                cb(Errno::Ok, order);
                return;
            } catch (const std::exception& ex) {
                INFRA_LOG_WARN("[toobit] [{}], exception: {}", name, ex.what());
            }
        } while (0);
        INFRA_LOG_WARN("[toobit] [{}] [fail], recv: {}", name, response);
        order->ec = extract_error_code(response);
        order->detail = response;
        order->status = OrderStatus::Failed;
        order->milli = time_get_now_milli();
        cb(order->ec, order);
    });
}
} // namespace infra