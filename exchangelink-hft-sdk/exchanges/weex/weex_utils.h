#pragma once
#include "common/logger.h"
#include "common/interface.h"
#include "exchanges/exchange_utils.h"
#include "network/rest.h"
#include "exchanges/signature.h"

namespace infra::weex {
// REST请求成功代码
// TODO: 确认 weex 的成功码字段名和值
inline std::string_view SUCCESS_CODE = "200";

// 缓存变量
inline UMSymbolExInfo g_pairs_info_cache;
inline Symbols g_all_symbols;
inline constexpr size_t MAX_PAIRS_PER_WS_CONNECTION = 80;

// 通用函数
inline Symbol transfer_from_infra_pair(const Symbol& pair) { return to_exchange_pair(Exchange::WEEX, pair); }
inline Symbol transfer_to_infra_pair(std::string_view pair) { return to_infra_pair(Exchange::WEEX, pair); }

Errno extract_error_code(std::string_view sv);
HttpRequestBody get_request_body_with_sign(boost::beast::http::verb method, const std::string& host,
                                           const std::string& path, const std::string& query, const std::string& body,
                                           const AccountSecret& secret);

// 解析函数
void parse_balance(const simdjson::dom::element& doc, const Currency& currency, UMCurrencyBalance& res);
void parse_position(const simdjson::dom::element& doc, UMSymbolPosition& res);
SpOrder parse_rtn_order(const simdjson::dom::object& obj, bool is_query = false);
SpFundingFee parse_funding_fee(const simdjson::dom::element& doc);
void parse_pairs_info(const simdjson::dom::element& doc, const Currency& currency);

inline APIConfig g_config_key_1 = {Exchange::WEEX, AccountType::SWAP, AddressType::NORMAL, Settlement::USDT};
inline UMExchangeConfig g_config_map = {{g_config_key_1.to_str(),
                                         {{REST_HOST, "api-contract.weex.com"},
                                          {WSS_PORT, "443"},
                                          {WSS_PUBLIC_HOST, "ws-contract.weex.com"},
                                          {WSS_PUBLIC_PATH, "/v3/ws/public"},
                                          {WSS_PRIVATE_HOST, "ws-contract.weex.com"},
                                          {WSS_PRIVATE_PATH, "/v3/ws/private"},
                                          {PAIRS_INFO_PATH, "/capi/v3/market/exchangeInfo"},
                                          {FUNDING_FEE_PATH, "/capi/v3/market/premiumIndex"},
                                          {BALANCE_PATH, "/capi/v3/account/balance"},
                                          {POSITION_PATH, "/capi/v3/account/position"},
                                          {LEVERAGE_PATH, "/capi/v3/account/leverage"},
                                          {QUERY_ORDER_PATH_PATH, "/capi/v3/order"},
                                          {CANCEL_ORDER_PATH_PATH, "/capi/v3/order"},
                                          {PLACE_ORDER_PATH_PATH, "/capi/v3/order"}}}};
} // namespace infra::weex
