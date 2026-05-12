#pragma once
#include "common/logger.h"
#include "common/interface.h"
#include "exchanges/exchange_utils.h"
#include "network/rest.h"

namespace infra::bitget {
inline Symbols g_all_symbols;
inline UMSymbolExInfo g_pairs_info_cache;

inline std::string_view SUCCESS_CODE = "00000";
inline std::string_view WS_SUCCESS_CODE = "0";

inline int64_t generate_req_id() {
    static int64_t g_req_id_ = 0;
    return ++g_req_id_;
}

// 回调处理模板
template <typename ParseFn, typename FailFn>
void handle_rest_response(HttpResponseBody& res, const std::string& msg, const char* func_name, ParseFn&& on_success,
                          FailFn&& on_fail) {
    do {
        if (res.result() != HTTP_STATUS_OK)
            break;
        try {
            PARSE_JSON(msg, doc);
            if (doc["code"].error() == simdjson::SUCCESS && doc["code"].get_string() != SUCCESS_CODE)
                break;
            if (on_success(doc))
                return;
        } catch (const std::exception& ex) {
            INFRA_LOG_WARN("[bitget] [{}]  [exception], ex: {}", func_name, ex.what());
        }
    } while (0);
    INFRA_LOG_WARN("[bitget] [{}] [fail], recv: {}", func_name, msg);
    on_fail();
}

Errno extract_error_code(std::string_view sv);
HttpRequestBody get_request_body_with_sign(boost::beast::http::verb method, const std::string& host,
                                           const std::string& path, const std::string& query,
                                           const AccountSecret& secret);

// 解析函数
void parse_funding_fee(const simdjson::dom::element& doc, SpFundingFee& res);
void parse_pairs_info(const simdjson::dom::element& doc, const Currency& currency);
void parse_balance(const simdjson::dom::element& doc, const Currency& currency, UMCurrencyBalance& res);
void parse_position(const simdjson::dom::element& doc, UMSymbolPosition& res);
double parse_margin_ratio(const simdjson::dom::element& doc);
SpOrder parse_rtn_order(const simdjson::dom::object& obj);

// 转换函数
inline Symbol transfer_from_infra_pair(const Symbol& pair) {
    static std::unordered_map<Symbol, Symbol> cache;
    auto it = cache.find(pair);
    if (it != cache.end()) [[likely]]
        return it->second;
    return cache[pair] = to_exchange_pair(Exchange::BITGET, pair);
}

inline Symbol transfer_to_infra_pair(std::string_view pair) {
    static std::unordered_map<std::string, Symbol> cache;
    auto it = cache.find(std::string(pair));
    if (it != cache.end()) [[likely]]
        return it->second;
    return cache[std::string(pair)] = to_infra_pair(Exchange::BITGET, pair);
}

// 配置信息
inline APIConfig g_config_key_1 = {Exchange::BITGET, AccountType::SWAP, AddressType::NORMAL, Settlement::USDT};
inline UMExchangeConfig g_config_map = {{g_config_key_1.to_str(),
                                         {{REST_HOST, "api.bitget.com"},
                                          {WSS_PORT, "443"},
                                          {WSS_PUBLIC_HOST, "ws.bitget.com"},
                                          {WSS_PUBLIC_PATH, "/v3/ws/public/sbe"},
                                          {WSS_PRIVATE_HOST, "ws.bitget.com"},
                                          {WSS_PRIVATE_PATH, "/v3/ws/private"},
                                          {PAIRS_INFO_PATH, "/api/v3/market/instruments"},
                                          {FUNDING_FEE_PATH, "/api/v3/market/current-fund-rate"},
                                          {BALANCE_PATH, "/api/v3/account/assets"},
                                          {POSITION_PATH, "/api/v3/position/current-position"},
                                          {LEVERAGE_PATH, "/api/v3/account/set-leverage"},
                                          {PLACE_ORDER_PATH_PATH, "/api/v3/trade/place-order"},
                                          {CANCEL_ORDER_PATH_PATH, "/api/v3/trade/cancel-order"},
                                          {QUERY_ORDER_PATH_PATH, "/api/v3/trade/order-info"}}}};
} // namespace infra::bitget
