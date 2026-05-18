#pragma once
#include "common/logger.h"
#include "common/interface.h"
#include "network/rest.h"
#include "exchanges/exchange_utils.h"
#include "exchanges/signature.h"
namespace infra::bybit {
// REST请求成功代码
constexpr int64_t BYBIT_SUCCESS_CODE = 0;

// 缓存变量
inline UMSymbolExInfo g_pairs_info_cache;
inline Symbols g_all_symbols;                             // 订阅全量行情时使用
inline constexpr size_t MAX_PAIRS_PER_WS_CONNECTION = 80; // 单个连接订阅个数

// 通用函数
inline Symbol transfer_from_infra_pair(const Symbol& pair) { return to_exchange_pair(Exchange::BYBIT, pair); }
inline Symbol transfer_to_infra_pair(std::string_view pair) { return to_infra_pair(Exchange::BYBIT, pair); }

Currency get_right_currency(const Currency& currency);
Errno extract_error_msg(std::string_view sv);
HttpRequestBody get_request_body_with_sign(boost::beast::http::verb method, const std::string& host,
                                           const std::string& path, const std::string& query,
                                           const AccountSecret& secret);

// 解析函数
void parse_balance(const simdjson::dom::element& doc, const Currency& currency, UMCurrencyBalance& res);
void parse_position(const simdjson::dom::element& doc, UMSymbolPosition& res);
double parse_margin_ratio(const simdjson::dom::element& doc);
SpOrder parse_rtn_order(const simdjson::dom::object& obj);
SpFundingFee parse_funding_fee(const simdjson::dom::element& doc);
void parse_pairs_info(const simdjson::dom::element& doc, const Currency& currency);

// 配置信息
inline APIConfig g_config_key_1 = {Exchange::BYBIT, AccountType::SWAP, AddressType::NORMAL, Settlement::USDT};
inline UMExchangeConfig g_config_map = {{g_config_key_1.to_str(),
                                         {{REST_HOST, "api.bybit.com"},
                                          {WSS_PORT, "443"},
                                          {WSS_PUBLIC_HOST, "stream.bybit.com"},
                                          {WSS_PUBLIC_PATH, "/v5/public/linear"},
                                          {WSS_PRIVATE_HOST, "stream.bybit.com"},
                                          {WSS_PRIVATE_PATH, "/v5/private"},
                                          {WSS_TRADE_HOST, "stream.bybit.com"},
                                          {WSS_TRADE_PATH, "/v5/trade"},
                                          {PAIRS_INFO_PATH, "/v5/market/instruments-info"},
                                          {FUNDING_FEE_PATH, "/v5/market/funding/history"},
                                          {BALANCE_PATH, "/v5/account/wallet-balance"},
                                          {POSITION_PATH, "/v5/position/list"},
                                          {LEVERAGE_PATH, "/v5/position/set-leverage"},
                                          {QUERY_ORDER_PATH_PATH, "/v5/order/realtime"},
                                          {PLACE_ORDER_PATH_PATH, "/v5/order/create"},
                                          {CANCEL_ORDER_PATH_PATH, "/v5/order/cancel"}}}};
} // namespace infra::bybit