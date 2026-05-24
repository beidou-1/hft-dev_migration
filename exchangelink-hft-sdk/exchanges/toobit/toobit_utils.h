#pragma once
#include "common/logger.h"
#include "common/interface.h"
#include "exchanges/exchange_utils.h"
#include "network/rest.h"
#include "exchanges/signature.h"

namespace infra::toobit {
// REST请求成功代码
constexpr int64_t SUCCESS_CODE = 200;

// 缓存变量
inline UMSymbolExInfo g_pairs_info_cache;
inline Symbols g_all_symbols;
inline constexpr size_t MAX_PAIRS_PER_WS_CONNECTION = 80; // 单个连接订阅个数
inline std::string g_token;

// 通用函数
inline Symbol transfer_from_infra_pair(const Symbol& pair) { return to_exchange_pair(Exchange::TOOBIT, pair); }
inline Symbol transfer_to_infra_pair(std::string_view pair) { return to_infra_pair(Exchange::TOOBIT, pair); }

double get_denomination_value(const Symbol& pair);
Errno extract_error_code(std::string_view sv);
HttpRequestBody get_request_body_with_sign(boost::beast::http::verb method, const std::string& host,
                                           const std::string& path, const std::string& query,
                                           const AccountSecret& secret);

// 解析函数
void parse_balance(const simdjson::dom::element& doc, const Currency& currency, UMCurrencyBalance& res);
void parse_position(const simdjson::dom::element& doc, UMSymbolPosition& res);
SpOrder parse_rtn_order(const simdjson::dom::object& obj, bool intact);
SpFundingFee parse_funding_fee(const simdjson::dom::element& doc);
void parse_pairs_info(const simdjson::dom::element& doc, const Currency& currency);
double parse_margin_ratio(const simdjson::dom::element& doc);

// 配置信息
inline APIConfig g_config_key_1 = {Exchange::TOOBIT, AccountType::SWAP, AddressType::NORMAL, Settlement::USDT};
inline UMExchangeConfig g_config_map = {{g_config_key_1.to_str(),
                                         {{REST_HOST, "api.toobit.com"},
                                          {WSS_PORT, "443"},
                                          {WSS_PUBLIC_HOST, "stream.toobit.com"},
                                          {WSS_PUBLIC_PATH, "/quote/ws/v1"},
                                          {WSS_PRIVATE_HOST, "stream.toobit.com"},
                                          {WSS_PRIVATE_PATH, "/api/v1/ws/"},
                                          {PAIRS_INFO_PATH, "/api/v1/exchangeInfo"},
                                          {FUNDING_FEE_PATH, "/api/v1/futures/fundingRate"},
                                          {BALANCE_PATH, "/api/v1/futures/balance"},
                                          {POSITION_PATH, "/api/v1/futures/positions"},
                                          {LEVERAGE_PATH, "/api/v1/futures/leverage"},
                                          {ORDER_PATH_PATH, "/api/v1/futures/order"},
                                          {LISTEN_KEY_PATH, "/api/v1/listenKey"}}}};
} // namespace infra::toobit