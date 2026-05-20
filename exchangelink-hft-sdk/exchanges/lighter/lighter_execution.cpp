#include "lighter_execution.h"
#include <boost/math/special_functions/round.hpp>
using namespace infra::lighter;

namespace infra {
bool LighterExecution::initialize() {
    auto& info = g_config_map[base_config_.to_str()];
    if (info.empty()) {
        INFRA_LOG_WARN("[lighter] [initialize] [fail], msg: {} {} {} not implemented",
                       to_string(base_config_.account_type), to_string(base_config_.address_type),
                       to_string(base_config_.settle_unit));
        return false;
    }

    if (!init_lighter_signer(account_secret_)) {
        INFRA_LOG_WARN("[lighter] [initialize] [fail], msg: init lighter signer failed");
        return false;
    }

    rest_host_ = info[REST_HOST];
    query_order_path_ = info[QUERY_ORDER_PATH_PATH];
    place_order_path_ = info[PLACE_ORDER_PATH_PATH];

    wss_config_ = {info[WSS_PRIVATE_HOST], info[WSS_PORT], info[WSS_PRIVATE_PATH]};
    wss_stream_.resolve_connect(wss_config_.host, wss_config_.port, wss_config_.path);
    INFRA_LOG_INFO("[lighter] [initialize] [Execution], websocket endpoint: {} {} {}", wss_config_.host,
                   wss_config_.path, wss_config_.port);
    return true;
}

void LighterExecution::shutdown() { wss_stream_.close(); }

void LighterExecution::query_order(const SpOrder order, OrderCallback cb) {
    if (order->market_oid.empty()) {
        INFRA_LOG_WARN("[lighter] [query_order] [fail], msg: market_oid is empty");
        cb(Errno::InvalidParams, order);
        return;
    }

    std::string query = "by=hash&value=" + order->market_oid;
    auto req = get_request_body(rest_host_, query_order_path_, query);
    send_http_request(req, order, cb, "query_order");
    INFRA_LOG_INFO("[lighter] [query_order], send: {}", query);
}

bool LighterExecution::subscribe_order(OrderCallback cb) {
    this->order_handler_ = std::move(cb);
    std::string payload = fmt::format(R"({{"type":"subscribe","channel":"account_all_orders/{}","auth":"{}"}})",
                                      g_account_index, g_api_token);
    INFRA_LOG_INFO("[lighter] [subscribe_order], send: {}", payload);
    send_ws_request(std::move(payload));
    return true;
}

void LighterExecution::unsubscribe_order() {
    this->order_handler_ = nullptr;
}

void LighterExecution::place_order(const SpOrder order, OrderCallback cb) {
    SignedTxResponse res;

    Symbol pair = transfer_from_infra_pair(order->pair);
    auto it = g_pairs_info_cache.find(pair);
    if (it == g_pairs_info_cache.end()) {
        INFRA_LOG_WARN("[lighter] [convert_place_order] [fail], msg: not found {} in cache", pair);
        order->ec = Errno::InvalidParams;
        order->detail = "pair not found in cache";
        order->status = OrderStatus::Failed;
        cb(Errno::InvalidParams, order);
        return;
    }

    SpExPairInfo pair_info = it->second;

    // SignCreateOrder 调用参数
    int cMarketIndex{0}, cPrice{0}, cIsAsk{0}, cOrderType{0}, cTimeInForce{0}, cReduceOnly{0}, cTriggerPrice{0};
    long long int cClientOrderIndex{0}, cBaseAmount{0}, cOrderExpiry{0}, cNonce{0};

    if (order->type == OrderType::Limit) {
        cOrderType = 0;
        switch (order->tif) {
            case OrderTIF::GTC:
                cTimeInForce = 1;
                cOrderExpiry = time_get_now_milli() + 16 * 24 * 60 * 60 * 1000; // 16 day
                break;
            case OrderTIF::MAKER:
                cTimeInForce = 2;
                cOrderExpiry = time_get_now_milli() + 16 * 24 * 60 * 60 * 1000; // 16 day
                break;
            case OrderTIF::IOC:
                cTimeInForce = 0;
                break;
            default:
                INFRA_LOG_WARN("[lighter] [convert_place_order] [fail], msg: {} not supported", to_string(order->tif));
                cb(Errno::InvalidParams, order);
                return;
        }
    } else if (order->type == OrderType::Market) {
        cOrderType = 1;
    } else {
        INFRA_LOG_WARN("[lighter] [convert_place_order] [fail], msg: order type is not supported");
        cb(Errno::InvalidParams, order);
        return;
    }

    cMarketIndex = std::stoi(pair_info->alias);
    cClientOrderIndex = std::stoll(order->client_oid);
    cBaseAmount = int(order->quantity / pair_info->step_size_base);
    cPrice = int(order->price / pair_info->step_size_quote);
    cIsAsk = (order->side == OrderSide::OpenLong || order->side == OrderSide::CloseShort) ? 0 : 1;
    cReduceOnly = (order->side == OrderSide::OpenLong || order->side == OrderSide::OpenShort) ? 0 : 1;
    cNonce = g_nonce_manager.get(rest_);

    res = SignCreateOrder(cMarketIndex, cClientOrderIndex, cBaseAmount, cPrice, cIsAsk, cOrderType, cTimeInForce,
                          cReduceOnly, cTriggerPrice, cOrderExpiry, cNonce, g_key_index, g_account_index);
    if (res.err != nullptr) {
        INFRA_LOG_WARN("[lighter] [convert_place_order] [fail], sign err:{}", res.err);
        cb(Errno::InvalidParams, order);
        return;
    }
    g_nonce_manager.update();

    order->market_oid = res.txHash;
    std::string payload =
        fmt::format(R"({{"type":"jsonapi/sendtx","data":{{"tx_type":{},"tx_info":{}}}}})", res.txType, res.txInfo);
    send_ws_request(std::move(payload));

    ws_request_cache_[order->market_oid] = std::make_pair(order, cb);
    INFRA_LOG_INFO("[lighter] [place_order_ws], txType:{}, txHash:{}, txInfo:{}", res.txType, res.txHash, res.txInfo);
}

void LighterExecution::cancel_order(const SpOrder order, OrderCallback cb) {
    SignedTxResponse res;

    if (!check_client_id(order->client_oid)) {
        INFRA_LOG_WARN("[lighter] [convert_cancel_order] [fail], msg: invalid client_oid, only digits are allowed");
        cb(Errno::InvalidParams, order);
        return;
    }

    Symbol pair = transfer_from_infra_pair(order->pair);
    auto it = g_pairs_info_cache.find(pair);
    if (it == g_pairs_info_cache.end()) {
        INFRA_LOG_WARN("[lighter] [convert_cancel_order] [fail], msg: not found {} in cache", pair);
        cb(Errno::InvalidParams, order);
        return;
    }

    // SignCancelOrder 调用参数
    int cMarketIndex = std::stoi(it->second->alias);
    long long int cOrderIndex = std::stoll(order->client_oid);
    long long int cNonce = g_nonce_manager.get(rest_);

    res = SignCancelOrder(cMarketIndex, cOrderIndex, cNonce, g_key_index, g_account_index);
    if (res.err != nullptr) {
        INFRA_LOG_WARN("[lighter] [convert_cancel_order] [fail], err: {}", res.err);
        cb(Errno::InvalidParams, order);
        return;
    }
    g_nonce_manager.update();

    order->market_oid = res.txHash;
    std::string payload =
        fmt::format(R"({{"type":"jsonapi/sendtx","data":{{"tx_type":{},"tx_info":{}}}}})", res.txType, res.txInfo);
    send_ws_request(std::move(payload));
    ws_request_cache_[order->market_oid] = std::make_pair(order, cb);
    INFRA_LOG_INFO("[lighter] [cancel_order_ws], txType:{}, txHash:{}, txInfo:{}", res.txType, res.txHash, res.txInfo);
}

Action LighterExecution::on_connect(Wss* ws) {
    INFRA_LOG_INFO("[lighter] [on_connect] [Execution], msg: WebSocket connection established");
    return Action::NONE;
}

Action LighterExecution::on_ping(Wss* ws, std::string_view payload) {
    // INFRA_LOG_DEBUG("[lighter] [on_ping] [Execution], payload: {}", payload);
    ws->pong(std::string(payload));
    return Action::NONE;
}

Action LighterExecution::on_pong(Wss* ws, std::string_view payload) {
    // INFRA_LOG_DEBUG("[lighter] [on_pong] [Execution], payload: {}", payload);
    return Action::NONE;
}

void LighterExecution::on_close(Wss* ws) {
    INFRA_LOG_WARN("[lighter] [on_close] [Execution], msg: WebSocket connection has been closed");
}

void LighterExecution::on_error(Wss* ws, std::string_view err) {
    INFRA_LOG_WARN("[lighter] [on_error] [Execution], msg: WebSocket error occurred: {}", err);
}

Action LighterExecution::on_message(Wss* ws, std::string_view msg) {
    // INFRA_LOG_DEBUG("[lighter] [on_message] [Execution], msg: {}", msg);
    try {
        PARSE_JSON(msg, doc);
        if (doc["type"].error() == simdjson::SUCCESS) {
            std::string_view type = doc["type"];
            if (type == "update/account_all_orders") {
                INFRA_LOG_INFO("[lighter] [on_message] [order], recv: {}", msg);
            } else if (type == "jsonapi/sendtx") {
                if (doc["code"].get_int64() != LIGHTER_SUCCESS_CODE) {
                    INFRA_LOG_INFO("[lighter] [on_message] [sendtx] [fail], msg: {}", msg);
                    if (doc["code"].get_int64() == 21104) {
                        // {"code":21104,"message":"invalid nonce"}
                        g_nonce_manager.peek(rest_);
                    }
                    return Action::RECEIVE;
                }

                std::string_view tx_hash = doc["tx_hash"];
                std::string uid(tx_hash);
                auto iter = ws_request_cache_.find(uid);
                if (iter == ws_request_cache_.end()) {
                    INFRA_LOG_WARN("[lighter] [on_message] [sendtx] [fail], msg:", msg);
                    return Action::RECEIVE;
                }
                INFRA_LOG_INFO("[lighter] [on_message] [sendtx] [success], recv: {}", msg);
                auto [order, cb] = ws_request_cache_[uid];
                order->status = OrderStatus::New;
                order->milli = time_get_now_milli();
                cb(Errno::Ok, order);
                ws_request_cache_.erase(iter);
            } else if (type == "subscribed/account_all_orders") {
                INFRA_LOG_INFO("[lighter] [subscribed] [success], recv: {}", msg);
            } else if (type == "connected") {
                INFRA_LOG_INFO("[lighter] [on_message] [system], recv: {}", msg);
            } else if (type == "ping") {
                return keep_ws_connection_alive();
            } else {
                INFRA_LOG_WARN("[lighter] [on_message] unexpected msg: {}", msg);
            }
        } else {
            INFRA_LOG_WARN("[lighter] [on_message] unexcepted msg: {}", msg);
        }
    } catch (const std::exception& ex) {
        INFRA_LOG_WARN("[lighter] [on_message] [exception], error: {}, msg: {}", ex.what(), msg);
    }
    return Action::RECEIVE;
}

void LighterExecution::login() {}

Action LighterExecution::keep_ws_connection_alive() {
    if (wss_stream_.is_socket_open()) {
        wss_stream_.send(R"({"type":"pong"})");
    } else {
        INFRA_LOG_WARN("[lighter] [keep_ws_connection_alive] [fail], msg: WebSocket not connected");
    }
    return Action::RECEIVE;
}

bool LighterExecution::convert_cancel_order(SpOrder order, OrderCallback cb, SignedTxResponse& res) {
    if (order->client_oid.empty() || order->pair.empty()) {
        INFRA_LOG_WARN("[bitmart] [convert_cancel_order] [fail], msg: market_oid or pair is empty");
        cb(Errno::InvalidParams, order);
        return false;
    }

    if (!check_client_id(order->client_oid)) {
        INFRA_LOG_WARN("[lighter] [convert_cancel_order] [fail], msg: invalid client_oid, only digits are allowed");
        cb(Errno::InvalidParams, order);
        return false;
    }

    Symbol pair = transfer_from_infra_pair(order->pair);
    auto it = g_pairs_info_cache.find(pair);
    if (it == g_pairs_info_cache.end()) {
        INFRA_LOG_WARN("[lighter] [convert_cancel_order] [fail], msg: not found {} in cache", pair);
        cb(Errno::InvalidParams, order);
        return false;
    }

    // SignCancelOrder 调用参数
    int cMarketIndex = std::stoi(it->second->alias);
    long long int cOrderIndex = std::stoll(order->client_oid);
    long long int cNonce = g_nonce_manager.get(rest_);

    res = SignCancelOrder(cMarketIndex, cOrderIndex, cNonce, g_key_index, g_account_index);
    if (res.err != nullptr) {
        INFRA_LOG_WARN("[lighter] [convert_cancel_order] [fail], err: {}", res.err);
        cb(Errno::InvalidParams, order);
        return false;
    }
    g_nonce_manager.update();
    return true;
}

void LighterExecution::send_http_request(const HttpRequestBody& req, SpOrder order, OrderCallback cb,
                                         std::string_view name) {
    rest_.send(req, [this, order, cb, name](HttpResponseBody& res) {
        std::string msg = boost::beast::buffers_to_string(res.body().data());
        do {
            if (res.result() != HTTP_STATUS_OK) {
                break;
            }
            try {
                PARSE_JSON(msg, doc);
                if (doc["code"].get_int64() != LIGHTER_SUCCESS_CODE) {
                    if (doc["code"].get_int64() == 21104) {
                        // {"code":21104,"message":"invalid nonce"}
                        g_nonce_manager.peek(rest_);
                    }
                    break;
                }
                if (name == "place_order_rest") {
                    order->status = OrderStatus::New;
                } else if (name == "cancel_order_rest") {
                    order->status = OrderStatus::Canceling;
                } else if (name == "query_order") {
                    simdjson::dom::object obj = doc.get_object();
                    SpOrder rtn_order = parse_tx_order(obj);
                    order->update(*rtn_order);
                }
                INFRA_LOG_INFO("[lighter] [{}] [success], recv: {}", name, msg);
                order->milli = time_get_now_milli();
                cb(Errno::Ok, order);
                return;
            } catch (const std::exception& ex) {
                INFRA_LOG_WARN("[lighter] [{}], exception: {}", name, ex.what());
            }
        } while (0);
        INFRA_LOG_WARN("[lighter] [{}] [fail], recv: {}", name, msg);
        order->ec = extract_error_code(msg);
        order->detail = msg;
        order->status = OrderStatus::Failed;
        order->milli = time_get_now_milli();
        cb(order->ec, order);
    });
}

void LighterExecution::send_ws_request(std::string&& content) {
    if (wss_stream_.is_socket_open()) {
        wss_stream_.send(std::move(content));
    } else {
        INFRA_LOG_WARN("[lighter] [send_ws_request] [fail], msg: WebSocket not connected");
    }
}
} // namespace infra