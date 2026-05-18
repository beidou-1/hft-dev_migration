#pragma once
#include "common/json.h"
#include "common/logger.h"
#include "common/interface.h"
#include "network/rest_client.h"
#include "network/wss_client.h"

namespace infra::kucoin {
// REST请求成功代码
constexpr const char* KUCOIN_SUCCESS_CODE = "200000";

// 缓存变量
inline UMSymbolExInfo g_pairs_info_cache;
inline Symbols g_all_symbols;
inline constexpr size_t MAX_PAIRS_PER_WS_CONNECTION = 60; // 单个连接订阅个数
inline PositionMode g_current_position_mode = PositionMode::one_way_mode;
inline MarginMode g_default_margin_mode = MarginMode::CROSS;
inline std::unordered_map<Symbol, MarginMode> g_current_symbol_margin_mode{};

// 默认为统一账户
inline AccountMode g_account_mode = AccountMode::UNIFIED;

// 通用函数
inline Symbol transfer_from_infra_pair(const Symbol& pair) { return to_exchange_pair(Exchange::KUCOIN, pair); }
inline Symbol transfer_to_infra_pair(std::string_view pair) { return to_infra_pair(Exchange::KUCOIN, pair); }
inline void keep_ws_connection_alive(WebSocketClient& client) {
    std::string msg = fmt::format(R"({{"id":"{}","type":"ping"}})", time_get_now_micro());
    client.start_ping_pong(msg, 15); // 心跳检测时间为20秒
}

bfloat get_denomination_value(const Symbol& pair);
Currency get_right_currency(const Currency& currency);
Errno extract_error_code(std::string_view sv);
HttpRequestBody get_request_body_with_sign(boost::beast::http::verb method, const std::string& host,
                                           const std::string& path, const std::string& query,
                                           const AccountSecret& secret);

// 解析函数(classic)
void parse_classic_balance(const simdjson::dom::element& doc, const Currency& currency, UMCurrencyBalance& res);
void parse_classic_position(const simdjson::dom::element& doc, const Symbol& symbol, UMSymbolPosition& res);
void parse_classic_position_object(const simdjson::dom::object& obj, const Symbol& symbol, UMSymbolPosition& res);

SpOrder parse_classic_query_order(const simdjson::dom::object& obj);
SpOrder parse_classic_rtn_order(const simdjson::dom::object& obj);
SpFundingFee parse_classic_funding_fee(const simdjson::dom::element& doc, const Symbol& symbol);
void parse_classic_pairs_info(const simdjson::dom::element& doc, const Currency& currency);

// 解析函数(unified)
void parse_unified_balance(const simdjson::dom::element& doc, const Currency& currency, UMCurrencyBalance& res);
void parse_unified_position(const simdjson::dom::element& doc, const Symbol& symbol, UMSymbolPosition& res);
void parse_unified_position_object(const simdjson::dom::object& obj, const Symbol& symbol, UMSymbolPosition& res);

SpOrder parse_unified_query_order(const simdjson::dom::object& obj);
SpOrder parse_unified_rtn_order(const simdjson::dom::object& obj);
SpFundingFee parse_unified_funding_fee(const simdjson::dom::element& doc, const Symbol& symbol);
void parse_unified_pairs_info(const simdjson::dom::element& doc, const Currency& currency);

std::string get_ws_url(const AccountSecret& secret);

// 配置信息
inline APIConfig g_config_key = {Exchange::KUCOIN, AccountType::SWAP, AddressType::NORMAL, Settlement::USDT};
inline UMExchangeConfig g_config_map = {{"classic",
                                         {{REST_HOST, "api-futures.kucoin.com"},
                                          {WSS_PORT, "443"},
                                          {WSS_PUBLIC_HOST, "ws-api-futures.kucoin.com"},
                                          {WSS_PUBLIC_PATH, "//?token={}&connectId={}"},
                                          {WSS_PRIVATE_HOST, "ws-api-futures.kucoin.com"},
                                          {WSS_PRIVATE_PATH, "//?token={}&connectId={}"},
                                          {WSS_TRADE_HOST, "wsapi.kucoin.com"},
                                          {WSS_TRADE_PATH, "/v1/private?{}"},
                                          {PAIRS_INFO_PATH, "/api/v1/contracts/active"},
                                          {FUNDING_FEE_PATH, "/api/v1/funding-rate/{}/current"},
                                          {BALANCE_PATH, "/api/v1/account-overview"},
                                          {POSITION_PATH, "/api/v1/positions"},
                                          {LEVERAGE_PATH, "/api/v2/changeCrossUserLeverage"},
                                          {MARGIN_MODE_PATH, "/api/v2/position/changeMarginMode"},
                                          {POSITION_MODE_PATH, "/api/v2/position/switchPositionMode"},
                                          {QUERY_ORDER_PATH_PATH, "/api/v1/orders/{}"},
                                          {CANCEL_ORDER_PATH_PATH, "/api/v1/orders/{}"},
                                          {ORDER_PATH_PATH, "/api/v1/orders"}}},
                                        {g_config_key.to_str(),
                                         {{REST_HOST, "api.kucoin.com"},
                                          {WSS_PORT, "443"},
                                          {WSS_PUBLIC_HOST, "x-push-futures.kucoin.com"},
                                          {WSS_PUBLIC_PATH, "/"},
                                          {WSS_PRIVATE_HOST, "wsapi-push.kucoin.com"},
                                          {WSS_PRIVATE_PATH, "/?token={}"},
                                          {WSS_TRADE_HOST, "wsapi.kucoin.com"},
                                          {WSS_TRADE_PATH, "/v1/private?{}"},
                                          {PAIRS_INFO_PATH, "/api/ua/v1/market/instrument"},
                                          {FUNDING_FEE_PATH, "/api/ua/v1/market/funding-rate"},
                                          {BALANCE_PATH, "/api/ua/v1/unified/account/balance"},
                                          {POSITION_PATH, "/api/ua/v1/unified/position/open-list"},
                                          {LEVERAGE_PATH, "/api/ua/v1/unified/account/modify-leverage"},
                                          {MARGIN_MODE_PATH, ""},
                                          {POSITION_MODE_PATH, ""},
                                          {QUERY_ORDER_PATH_PATH, "/api/ua/v1/unified/order/detail"},
                                          {CANCEL_ORDER_PATH_PATH, "/api/ua/v1/unified/order/cancel"},
                                          {ORDER_PATH_PATH, "/api/ua/v1/unified/order/place"}}}};
} // namespace infra::kucoin