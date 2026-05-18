#pragma once
// #include "common/json.h"
#include "common/logger.h"
#include "common/interface.h"
#include <boost/algorithm/string.hpp>
#include <sstream>
#include <iomanip>
#include <cctype>
#include "exchanges/exchange_utils.h"
#include "network/rest.h"
#include "exchanges/signature.h"

namespace infra::hbg {
// REST请求成功代码
constexpr int64_t SUCCESS_CODE = 0;

// 缓存变量
inline UMSymbolExInfo g_pairs_info_cache;
inline Symbols g_all_symbols;
inline MarginMode g_current_margin_mode = MarginMode::CROSS;
inline std::string g_base_host;

template <typename ParseFn, typename FailFn>
void handle_rest_response(HttpResponseBody& res, const std::string& msg, const char* func_name, ParseFn&& on_success,
                          FailFn&& on_fail) {
    do {
        if (res.result() != HTTP_STATUS_OK)
            break;
        try {
            PARSE_JSON(msg, doc);
            
            if (on_success(doc))
                return;
        } catch (const std::exception& ex) {
            INFRA_LOG_WARN("[hbg] [{}] [exception], ex: {}", func_name, ex.what());
        }
    } while (0);
    INFRA_LOG_WARN("[hbg] [{}] [fail], recv: {}", func_name, msg);
    on_fail();
}

// 通用函数
inline Symbol transfer_from_infra_pair(const Symbol& pair) { 
    static std::unordered_map<Symbol, Symbol> cache;
    auto it = cache.find(pair);
    if (it != cache.end()) [[likely]]
        return it->second;
    return cache[pair] = to_exchange_pair(Exchange::HBG, pair); 
}

inline Symbol transfer_to_infra_pair(std::string_view pair) { 
    static std::unordered_map<std::string, Symbol> cache;
    auto it = cache.find(std::string(pair));
    if (it != cache.end()) [[likely]]
        return it->second;
    return cache[std::string(pair)] = to_infra_pair(Exchange::HBG, pair); 
}

Errno extract_error_code(std::string_view sv);
HttpRequestBody get_request_body_with_sign(boost::beast::http::verb method, const std::string& host,
                                           const std::string& path, const std::string& query, const std::string& body,
                                           const AccountSecret& secret);

std::string get_websocket_sign(const std::string& host, const std::string& path, const std::string& query,
                               const std::string& op, const std::string& time_str, const AccountSecret& secret);
std::string hbg_decompress_gzip(std::string_view input);
Currency get_right_currency(const Currency& currency);

// 解析函数
void parse_balance(const simdjson::dom::element& doc, const Currency& currency, UMCurrencyBalance& res);
void parse_position(const simdjson::dom::element& doc, UMSymbolPosition& res);
SpOrder parse_rtn_order(const simdjson::dom::object& obj, const std::string& channel);
SpFundingFee parse_funding_fee(const simdjson::dom::element& doc, const Symbol& pair);
void parse_pairs_info(const simdjson::dom::element& doc, const Currency& currency);
double parse_margin_ratio(const simdjson::dom::element& doc);

// 配置信息
inline APIConfig g_config_key_1 = {Exchange::HBG, AccountType::SWAP, AddressType::NORMAL, Settlement::USDT};
inline UMExchangeConfig g_config_map = {{g_config_key_1.to_str(),
                                         {{REST_HOST, "api.hbdm.vn"},
                                          {WSS_PORT, "443"},
                                          {WSS_PUBLIC_HOST, "api.hbdm.vn"},
                                          {WSS_PUBLIC_PATH, "/linear-swap-ws"},
                                          {WSS_PRIVATE_HOST, "api.hbdm.vn"},
                                          {WSS_PRIVATE_PATH, "/ws/v5/notification"},
                                          {WSS_TRADE_HOST, "api.hbdm.vn"},
                                          {WSS_TRADE_PATH, "/linear-swap-trade"},
                                          {PAIRS_INFO_PATH, "/linear-swap-api/v1/swap_contract_info"},
                                          {FUNDING_FEE_PATH, "/linear-swap-api/v1/swap_funding_rate"},
                                          {BALANCE_PATH, "/v5/account/balance"},
                                          {POSITION_PATH, "/v5/trade/position/opens"},
                                          {LEVERAGE_PATH, "/v5/position/lever"},
                                          {"host_for_sign", "api.hbdm.vn"},
                                          {ORDER_PATH_PATH, "/v5/trade/order"},
                                          {QUERY_ORDER_PATH_PATH, "/v5/trade/order"},
                                          {CANCEL_ORDER_PATH_PATH, "/v5/trade/cancel_order"}}}};
} // namespace infra::hbg