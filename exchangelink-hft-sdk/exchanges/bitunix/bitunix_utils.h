#pragma once
#include "common/logger.h"
#include "common/interface.h"
#include "exchanges/exchange_utils.h"
#include "network/rest.h"
#include "exchanges/signature.h"

namespace infra::bitunix {
// REST请求成功代码
constexpr int64_t SUCCESS_CODE = 0;

// 缓存变量
inline UMSymbolExInfo g_pairs_info_cache;
inline Symbols g_all_symbols;
inline constexpr size_t MAX_PAIRS_PER_WS_CONNECTION = 80; // 单个连接订阅个数
inline PositionMode g_current_position_mode = PositionMode::one_way_mode;

// 通用函数
inline Symbol transfer_from_infra_pair(const Symbol& pair) { return to_exchange_pair(Exchange::BITUNIX, pair); }
inline Symbol transfer_to_infra_pair(std::string_view pair) { return to_infra_pair(Exchange::BITUNIX, pair); }

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

// 配置信息
inline APIConfig g_config_key_1 = {Exchange::BITUNIX, AccountType::SWAP, AddressType::NORMAL, Settlement::USDT};
inline UMExchangeConfig g_config_map = {{g_config_key_1.to_str(),
                                         {{REST_HOST, "fapi.bitunix.com"},
                                          {WSS_PORT, "443"},
                                          {WSS_PUBLIC_HOST, "fapi.bitunix.com"},
                                          {WSS_PUBLIC_PATH, "/public/"},
                                          {WSS_PRIVATE_HOST, "fapi.bitunix.com"},
                                          {WSS_PRIVATE_PATH, "/private/"},
                                          {PAIRS_INFO_PATH, "/api/v1/futures/market/trading_pairs"},
                                          {FUNDING_FEE_PATH, "/api/v1/futures/market/funding_rate"},
                                          {BALANCE_PATH, "/api/v1/futures/account"},
                                          {POSITION_PATH, "/api/v1/futures/position/get_pending_positions"},
                                          {LEVERAGE_PATH, "/api/v1/futures/account/change_leverage"},
                                          {QUERY_ORDER_PATH_PATH, "/api/v1/futures/trade/get_order_detail"},
                                          {CANCEL_ORDER_PATH_PATH, "/api/v1/futures/trade/cancel_orders"},
                                          {PLACE_ORDER_PATH_PATH, "/api/v1/futures/trade/place_order"}}}};
} // namespace infra::bitunix