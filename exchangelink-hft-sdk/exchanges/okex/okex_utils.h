#pragma once
#include "common/logger.h"
#include "common/interface.h"
#include "exchanges/exchange_utils.h"
#include "network/rest.h"
#include "exchanges/signature.h"

namespace infra::okex {

inline UMSymbolExInfo g_pairs_info_cache;
inline Symbols g_all_symbols;
inline constexpr size_t MAX_PAIRS_PER_WS_CONNECTION = 80; // 单个连接订阅个数

inline Symbol transfer_from_infra_pair(const Symbol& pair) { return to_exchange_pair(Exchange::OKEX, pair); }
inline Symbol transfer_to_infra_pair(std::string_view pair) { return to_infra_pair(Exchange::OKEX, pair); }

inline Errno extract_error_msg(std::string_view sv) {
    if (sv.find("timeout") != std::string_view::npos) {
        return Errno::RequestTimeout;
    } else if (sv.find("ordId error") != std::string_view::npos) {
        return Errno::OrderNotFound;
    } else if (sv.find("not exist") != std::string_view::npos) {
        return Errno::OrderNotFound;
    } else if (sv.find("Invalid args") != std::string_view::npos) {
        return Errno::InvalidParams;
    } else if (sv.find("Parameter") != std::string_view::npos) {
        return Errno::InvalidParams;
    } else if (sv.find("API-key") != std::string_view::npos) {
        return Errno::AuthFailed;
    } else if (sv.find("insufficient") != std::string_view::npos) {
        return Errno::InsufficientBalance;
    } else {
        return Errno::UnknownError;
    }
}

inline HttpRequestBody get_request_body_with_sign(boost::beast::http::verb method, const std::string& host, const std::string& path,
                                                  const std::string& query, const AccountSecret& secret) {
    std::string url_str{};
    std::string request_body = query;
    using namespace boost::beast;
    if (method == http::verb::get) {
        url_str = query.empty() ? path : (path + "?" + query);
    } else if (method == http::verb::post) {
        url_str = path;
    }

    std::string raw_str{};
    std::string timestamp = time_milli_to_iso(time_get_now_milli());
    std::string method_text = "";
    std::string query_text = query;
    if (method == http::verb::get) {
        method_text = "GET";
        query_text = "?" + query_text;
    } else if (method == http::verb::post) {
        method_text = "POST";
    }
    raw_str.append(timestamp).append(method_text).append(path);
    if (!query.empty()) {
        raw_str.append(query_text);
    }
    std::string signature = generate_sign_hmac256_b64(secret.api_secret, raw_str);

    HttpRequestBody req{method, url_str, 11};
    req.set(http::field::host, host);
    req.set(http::field::user_agent, BOOST_BEAST_VERSION_STRING);

    req.set("OK-ACCESS-KEY", secret.api_key);
    req.set("OK-ACCESS-SIGN", signature);
    req.set("OK-ACCESS-TIMESTAMP", timestamp);
    req.set("OK-ACCESS-PASSPHRASE", secret.api_phrase);

    if (method == http::verb::get) {
        req.set(http::field::content_type, "application/x-www-form-urlencoded");
    } else if (method == http::verb::post) {
        req.set(http::field::content_type, "application/json");
        req.body() = request_body;
        req.prepare_payload();
    }
    return req;
}

inline double get_denomination_value(const Symbol& pair) {
    auto it = g_pairs_info_cache.find(pair);
    if (it != g_pairs_info_cache.end() && it->second != nullptr) {
        return it->second->denomination_value;
    }
    return 0;
}

// 解析函数
void parse_balance(const Currency& currency, const std::string& raw_str, UMCurrencyBalance& res);
void parse_position(const std::string& raw_str, UMSymbolPosition& res);
double parse_margin_ratio(const simdjson::dom::element& doc);
SpOrder parse_rtn_order(const simdjson::dom::object& obj);
SpFundingFee parse_funding_fee(const std::string& raw_str);
void parse_pairs_info(const std::string& raw_str, const Currency& currency);

// 配置信息
inline APIConfig g_config_key_1 = {Exchange::OKEX, AccountType::SWAP, AddressType::NORMAL, Settlement::USDT};
inline UMExchangeConfig g_config_map = {{g_config_key_1.to_str(),
                                         {{REST_HOST, "www.okx.com"},
                                          {WSS_PORT, "8443"},
                                          {WSS_PUBLIC_HOST, "ws.okx.com"},
                                          {WSS_PUBLIC_PATH, "/ws/v5/public"},
                                          {WSS_PRIVATE_HOST, "ws.okx.com"},
                                          {WSS_PRIVATE_PATH, "/ws/v5/private"},
                                          {PAIRS_INFO_PATH, "/api/v5/public/instruments"},
                                          {FUNDING_FEE_PATH, "/api/v5/public/funding-rate"},
                                          {BALANCE_PATH, "/api/v5/account/balance"},
                                          {POSITION_PATH, "/api/v5/account/positions"},
                                          {LEVERAGE_PATH, "/api/v5/account/set-leverage"},
                                          {QUERY_ORDER_PATH_PATH, "/api/v5/trade/order"},
                                          {PLACE_ORDER_PATH_PATH, "/api/v5/trade/order"},
                                          {CANCEL_ORDER_PATH_PATH, "/api/v5/trade/cancel-order"}}}};
} // namespace infra::okx
