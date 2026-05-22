#include "edgex_execution.h"
using namespace infra::edgex;

namespace infra {
bool EdgexExecution::initialize() {
    auto& info = g_config_map[base_config_.to_str()];
    if (info.empty()) {
        INFRA_LOG_WARN("[edgex] [initialize] [fail], msg: {} {} {} not implemented",
                       to_string(base_config_.account_type), to_string(base_config_.address_type),
                       to_string(base_config_.settle_unit));
        return false;
    }

    if (account_secret_.api_key.empty() || account_secret_.api_secret.empty() || account_secret_.api_phrase.empty()) {
        INFRA_LOG_WARN("[edgex] [initialize] [fail], msg: AccountSecret filed is empty");
        return false;
    }

    if (account_secret_.custom_info.count("account_id") == 0 || account_secret_.custom_info["account_id"].empty()) {
        INFRA_LOG_WARN("[edgex] [initialize] [fail], msg: AccountSecret filed: custom_info[account_id] is empty");
        return false;
    }
    g_account_id = account_secret_.custom_info["account_id"];

    rest_host_ = info[REST_HOST];
    query_order_path_ = info[QUERY_ORDER_PATH_PATH];
    place_order_path_ = info[PLACE_ORDER_PATH_PATH];
    cancel_order_path_ = info[CANCEL_ORDER_PATH_PATH];

    wss_config_ = {info[WSS_PRIVATE_HOST], info[WSS_PORT], info[WSS_PRIVATE_PATH]};
    INFRA_LOG_INFO("[edgex] [initialize] [Execution], websocket endpoint: {} {} {}", wss_config_.host, wss_config_.path,
                   wss_config_.port);
    return true;
}

void EdgexExecution::shutdown() { wss_stream_.close(); }

void EdgexExecution::query_order(const SpOrder order, OrderCallback cb) {
    if (order->market_oid.empty()) {
        INFRA_LOG_WARN("[edgex] [query_order] [fail], msg: market_oid is empty");
        cb(Errno::InvalidParams, order);
        return;
    }

    std::string query{};
    query.append("accountId=").append(g_account_id);
    query.append("&orderIdList=").append(order->market_oid);
    auto req = get_request_body_with_sign(HTTP_GET, rest_host_, query_order_path_, query, account_secret_);
    send_http_request(req, order, cb, "query_order");
    INFRA_LOG_INFO("[edgex] [query_order], send: {}", query);
}

void EdgexExecution::place_order(const SpOrder order, OrderCallback cb) {
    std::string payload{};
    std::string sorted_payload{};
    if (!convert_place_order(order, cb, payload, sorted_payload)) {
        return;
    }

    auto req = get_request_body_with_sign(HTTP_POST, rest_host_, place_order_path_, payload, account_secret_,
                                          sorted_payload);
    send_http_request(req, order, cb, "place_order");
    INFRA_LOG_INFO("[edgex] [place_order], send: {}", payload);
}

void EdgexExecution::cancel_order(const SpOrder order, OrderCallback cb) {
    if (order->market_oid.empty()) {
        INFRA_LOG_WARN("[edgex] [cancel_order] [fail], msg: market_oid is empty");
        cb(Errno::InvalidParams, order);
        return;
    }

    std::string payload =
        fmt::format("{{\"accountId\":\"{}\",\"orderIdList\":[\"{}\"]}}", g_account_id, order->market_oid);
    std::string sorted_payload = fmt::format("accountId={}&orderIdList={}", g_account_id, order->market_oid);
    auto req = get_request_body_with_sign(HTTP_POST, rest_host_, cancel_order_path_, payload,
                                          account_secret_, sorted_payload);
    send_http_request(req, order, cb, "cancel_order");
    INFRA_LOG_INFO("[edgex] [cancel_order], send: {}", payload);
}

bool EdgexExecution::subscribe_order(OrderCallback cb) {
    this->order_handler_ = std::move(cb);
    static std::string real_ws_path{}; // 使用static保证string_view生命周期
    real_ws_path = wss_config_.path + "?accountId=" + g_account_id;
    wss_stream_.set_sign_cb(std::bind(&EdgexExecution::sign_ws, this, std::placeholders::_1));
    wss_stream_.resolve_connect(wss_config_.host, wss_config_.port, real_ws_path);
    return true;
}

void EdgexExecution::unsubscribe_order() {
    this->order_handler_ = nullptr;
    wss_stream_.close();
}

Action EdgexExecution::on_connect(Wss* ws) {
    INFRA_LOG_INFO("[edgex] [on_connect] [Execution], msg: WebSocket connection established");
    keep_ws_connection_alive(wss_stream_);
    return Action::NONE;
}

Action EdgexExecution::on_ping(Wss* ws, std::string_view payload) {
    // INFRA_LOG_DEBUG("[edgex] [on_ping] [Execution], payload: {}", payload);
    ws->pong(std::string(payload));
    return Action::NONE;
}

Action EdgexExecution::on_pong(Wss* ws, std::string_view payload) {
    // INFRA_LOG_DEBUG("[edgex] [on_pong] [Execution], payload: {}", payload);
    return Action::NONE;
}

void EdgexExecution::on_close(Wss* ws) {
    INFRA_LOG_WARN("[edgex] [on_close] [Execution], msg: WebSocket connection has been closed");
}

void EdgexExecution::on_error(Wss* ws, std::string_view err) {
    INFRA_LOG_WARN("[edgex] [on_error] [Execution], msg: WebSocket error occurred: {}", err);
}

Action EdgexExecution::on_message(Wss* ws, std::string_view msg) {
    // INFRA_LOG_INFO("[edgex] [on_message] [Execution], msg: {}", msg);
    try {
        PARSE_JSON(msg, doc);
        if (doc["type"].error() == simdjson::SUCCESS) {
            std::string_view type = doc["type"];
            if (type == "trade-event") {
                simdjson::dom::object content = doc["content"];
                std::string_view event = content["event"];
                if (event == "ORDER_UPDATE") {
                    simdjson::dom::array orders = content["data"]["order"];
                    for (auto item : orders) {
                        SpOrder rtn_order = parse_rtn_order(item);
                        this->dispatch_order(rtn_order);
                    }
                    std::string orders_str = simdjson::minify(orders);
                    INFRA_LOG_INFO("[edgex] [on_message] [order], msg: {}", orders_str);
                } else if (event == "Snapshot" || event == "TX_L2_APPROVED") {
                    // ignore
                } else {
                    INFRA_LOG_WARN("[edgex] [on_message] [Execution] unexpected msg: {}", msg);
                }
            } else if (type == "connected" || type == "subscribed") {
                INFRA_LOG_INFO("[edgex] [on_message] [Execution] msg: {}", msg);
            } else if (type == "ping" || type == "pong") {
                // ignore
            } else {
                INFRA_LOG_WARN("[edgex] [on_message] [Execution] unexpected msg: {}", msg);
            }
        } else {
            INFRA_LOG_WARN("[edgex] [on_message] [Execution] unexpected msg: {}", msg);
        }
    } catch (const std::exception& ex) {
        INFRA_LOG_WARN("[edgex] [on_message] [exception], error: {}, msg: {}", ex.what(), msg);
    }
    return Action::RECEIVE;
}

void EdgexExecution::login() {
    // NOTE: 通过wss请求头做认证
}

bool EdgexExecution::convert_place_order(SpOrder order, OrderCallback cb, std::string& payload,
                                         std::string& sorted_payload) {
    if (order->type != OrderType::Limit && order->type != OrderType::Market) {
        INFRA_LOG_WARN("[edgex] [convert_place_order] [fail], msg: order type is not supported");
        cb(Errno::InvalidParams, order);
        return false;
    }

    if (order->client_oid.empty() || order->pair.empty()) {
        INFRA_LOG_WARN("[edgex] [convert_place_order] [fail], msg: client_oid or pair is empty");
        cb(Errno::InvalidParams, order);
        return false;
    }

    auto it = g_pairs_info_cache.find(to_lower_str(order->pair));
    if (it == g_pairs_info_cache.end()) {
        INFRA_LOG_WARN("[edgex] [convert_place_order] [fail], msg: not found {} in cache", order->pair);
        cb(Errno::InvalidParams, order);
        return false;
    }

    SpExPairInfo pair_info = it->second;
    auto contract_it = g_stark_info_cache.find(to_lower_str(order->pair));
    if (contract_it == g_stark_info_cache.end()) {
        INFRA_LOG_WARN("[edgex] [convert_place_order] [fail], msg: not found {} contract id stark info in cache",
                       order->pair);
        cb(Errno::InvalidParams, order);
        return false;
    }
    std::string quote_coin = "usd";
    auto coin_it = g_stark_info_cache.find(quote_coin);
    if (coin_it == g_stark_info_cache.end()) {
        INFRA_LOG_WARN("[edgex] [convert_place_order] [fail], msg: not found {} quote coin id stark info in cache",
                       quote_coin);
        cb(Errno::InvalidParams, order);
        return false;
    }
    std::map<std::string, std::string> params;
    params["symbol"] = transfer_from_infra_pair(order->pair);
    params["contractId"] = pair_info->alias;
    params["clientOrderId"] = order->client_oid;
    params["accountId"] = g_account_id;
    bool is_buy = false;
    if (order->side == OrderSide::OpenLong) {
        params["side"] = "BUY";
        is_buy = true;
    } else if (order->side == OrderSide::OpenShort) {
        params["side"] = "SELL";
    } else if (order->side == OrderSide::CloseLong) {
        params["side"] = "SELL";
        params["reduceOnly"] = "true";
    } else if (order->side == OrderSide::CloseShort) {
        params["side"] = "BUY";
        params["reduceOnly"] = "true";
        is_buy = true;
    }

    double quantity = int(order->quantity / pair_info->step_size_base) * pair_info->step_size_base;
    double price = int(order->price / pair_info->step_size_quote) * pair_info->step_size_quote;
    constexpr std::array<const char*, 5> tifToStr = {"GOOD_TIL_CANCEL", "POST_ONLY", "IMMEDIATE_OR_CANCEL",
                                                     "FILL_OR_KILL", "POC"};
    params["size"] = float_to_compact_str(quantity);

    double l2price;
    double value;
    switch (order->type) {
        case OrderType::Limit: {
            l2price = price;
            value = l2price * quantity;
            params["type"] = "LIMIT";
            params["price"] = float_to_compact_str(price);
            params["timeInForce"] = tifToStr[static_cast<uint8_t>(order->tif)];
            break;
        }
        case OrderType::Market: {
            params["type"] = "MARKET";
            params["price"] = "0";
            params["timeInForce"] = "IMMEDIATE_OR_CANCEL";
            if (is_buy) {
                l2price = get_maker_price(order->pair);
                l2price = int((l2price * 10) / pair_info->step_size_quote) * pair_info->step_size_quote;
                value = l2price * quantity;
            } else {
                l2price = pair_info->step_size_quote;
                value = l2price * quantity;
            }
            break;
        }
        default:
            INFRA_LOG_WARN("[edgex] [convert_place_order] [fail], msg: order type is not supported");
            cb(Errno::InvalidParams, order);
            return false;
    }

    // 准备签名参数
    uint64_t accountId = std::stoull(g_account_id);
    Timestamp expireTime = time_get_now_milli() + 24 * 60 * 60 * 1000; // 1天有效期
    Timestamp L2expireTime = expireTime + 9 * 24 * 60 * 60 * 1000;     // 10天有效期
    std::vector<uint8_t> nonce_bytes(SHA256_DIGEST_LENGTH);
    std::vector<uint8_t> clientOID_bytes(std::vector<uint8_t>(order->client_oid.begin(), order->client_oid.end()));
    SHA256(clientOID_bytes.data(), clientOID_bytes.size(), nonce_bytes.data());
    uint32_t nonce = (static_cast<uint32_t>(nonce_bytes[0]) << 24) | (static_cast<uint32_t>(nonce_bytes[1]) << 16) |
                     (static_cast<uint32_t>(nonce_bytes[2]) << 8) | (static_cast<uint32_t>(nonce_bytes[3]));
    double amount_synthetic_float = quantity * double(contract_it->second->starkResolution);
    double amount_collateral_float = value * double(coin_it->second->starkResolution);
    double taker_fee = value * contract_it->second->takerFee + 1;

    cpp_int amount_synthetic_int(amount_synthetic_float);
    cpp_int amount_collateral_int(amount_collateral_float);
    cpp_int taker_fee_int(taker_fee);
    cpp_int max_amount_fee = taker_fee_int * coin_it->second->starkResolution;
    std::string L2_signature =
        calc_limit_order_hash(contract_it->second->starkAssetId, coin_it->second->starkAssetId,
                              coin_it->second->starkAssetId, is_buy, amount_synthetic_int, amount_collateral_int,
                              max_amount_fee, nonce, accountId, L2expireTime, account_secret_.api_secret);
    params["l2Signature"] = L2_signature;
    params["l2Nonce"] = std::to_string(nonce);
    params["l2Size"] = params["size"];
    params["l2ExpireTime"] = std::to_string(L2expireTime);
    params["l2Value"] = float_to_compact_str(value);
    params["expireTime"] = std::to_string(expireTime);
    params["l2LimitFee"] = float_to_compact_str(double(taker_fee_int));
    std::string request_str{};
    request_str.reserve(256);
    request_str.append("{");
    for (const auto& [key, value] : params) {
        if (key != "reduceOnly")
            request_str.append("\"" + key + "\"").append(":").append("\"" + value + "\",");
        else
            request_str.append("\"" + key + "\"").append(":").append(value + ",");
    }
    request_str.pop_back();
    request_str.append("}");
    payload = request_str;
    sorted_payload = map_to_query_str(params);
    return true;
}

void EdgexExecution::send_http_request(const HttpRequestBody& req, SpOrder order, OrderCallback cb,
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
                std::string_view code_str = doc["code"];
                if (code_str != SUCCESS_CODE) {
                    break;
                }
                if (name == "place_order") {
                    std::string_view order_id = doc["data"]["orderId"];
                    order->market_oid = order_id;
                    order->status = OrderStatus::New;
                } else if (name == "cancel_order") {
                    order->status = OrderStatus::Canceling;
                } else if (name == "query_order") {
                    simdjson::dom::array orders = doc["data"];
                    if (orders.size() < 1) {
                        break;
                    }
                    for (auto order_obj : orders) {
                        SpOrder updated_order = parse_rtn_order(order_obj);
                        order->update(*updated_order);
                    }
                }
                INFRA_LOG_INFO("[edgex] [{}] [success], recv: {}", name, response);
                order->milli = time_get_now_milli();
                cb(Errno::Ok, order);
                return;
            } catch (const std::exception& ex) {
                INFRA_LOG_WARN("[edgex] [{}], exception: {}", name, ex.what());
            }
        } while (0);
        INFRA_LOG_WARN("[edgex] [{}] [fail], recv: {}", name, response);
        order->ec = extract_error_code(response);
        order->detail = response;
        order->status = OrderStatus::Failed;
        order->milli = time_get_now_milli();
        cb(order->ec, order);
    });
}

void EdgexExecution::sign_ws(websocket::request_type& req) {
    std::string timestamp = std::to_string(time_get_now_milli());
    std::string raw_str = timestamp + "GET" + wss_config_.path + "accountId=" + g_account_id;
    std::string signature = sign_edgex_params(raw_str, account_secret_.api_secret, account_secret_.api_phrase);
    std::transform(signature.begin(), signature.end(), signature.begin(), ::tolower);
    req.set("X-edgeX-Api-Signature", signature);
    req.set("X-edgeX-Api-Timestamp", timestamp);
    INFRA_LOG_INFO("[edgex] [sign_ws], raw_str:{}, signature: {}", raw_str, signature);
}

double EdgexExecution::get_maker_price(const Symbol& symbol) {
    if (symbol.empty()) {
        return 0.0;
    }

    std::string contractId;
    if (!get_contract_id(symbol, contractId)) {
        INFRA_LOG_WARN("[edgex] [get_maker_price] [fail], msg: get contract id failed for pair {}", symbol);
        return 0.0;
    }

    std::string query = "contractId=" + contractId;
    auto req = get_request_body(rest_host_, "/api/v1/public/quote/getTicker", query);
    double price = 0;
    boost::beast::error_code ec;
    std::string response = rest_.sync_send(req, ec);
    do {
        if (ec) {
            break;
        }
        try {
            PARSE_JSON(response, doc);
            if (doc["code"].error() != simdjson::SUCCESS) {
                break;
            }
            std::string_view code_str = doc["code"];
            if (code_str != SUCCESS_CODE) {
                break;
            }
            simdjson::dom::array symbols_array = doc["data"];
            if (symbols_array.size() == 0) {
                break;
            }
            simdjson::dom::object obj = *(symbols_array.begin());
            std::string_view oraclePrice = obj["oraclePrice"];
            price = str_to_float(oraclePrice);
            INFRA_LOG_INFO("[edgex] [get_maker_price] [success], recv: {}", price);
            return price;
        } catch (const std::exception& ex) {
            INFRA_LOG_WARN("[edgex] [get_maker_price] [exception], msg: {}", ex.what());
        }
    } while (0);
    INFRA_LOG_WARN("[edgex] [get_maker_price] [fail], recv: {}", response);
    return price;
}
} // namespace infra