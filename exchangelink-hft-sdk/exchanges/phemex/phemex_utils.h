#pragma once
#include "common/logger.h"
#include "common/interface.h"
#include "exchanges/exchange_utils.h"
#include "network/rest.h"
#include "exchanges/signature.h"

namespace infra::phemex {
// REST请求成功代码
constexpr int64_t SUCCESS_CODE = 0;
inline PositionMode g_current_position_mode = PositionMode::one_way_mode;
// 缓存变量
inline UMSymbolExInfo g_pairs_info_cache;
inline Symbols g_all_symbols;
inline constexpr size_t MAX_PAIRS_PER_WS_CONNECTION = 65; // 单个连接订阅个数

// 通用函数
inline Symbol transfer_from_infra_pair(const Symbol& pair) { return to_exchange_pair(Exchange::PHEMEX, pair); }
inline Symbol transfer_to_infra_pair(std::string_view pair) { return to_infra_pair(Exchange::PHEMEX, pair); }
inline void keep_ws_connection_alive(WebSocketClient& client) {
    client.start_ping_pong(R"({"id":1234, "method":"server.ping", "params":[]})", 25); // 心跳检测时间为30秒
}
Errno extract_error_code(std::string_view sv);
HttpRequestBody get_request_body_with_sign(boost::beast::http::verb method, const std::string& host,
                                           const std::string& path, const std::string& query,
                                           const AccountSecret& secret);
inline bool send_ws_request(WebSocketClient& client, const std::string& content, const std::string& name) {
    if (LIKELY(client.is_socket_open())) {
        client.send(content);
        INFRA_LOG_INFO("[phemex] [{}], send: {}", name, content);
        return true;
    } else {
        INFRA_LOG_WARN("[phemex] [{}] [fail], msg: WebSocket not connected", name);
        return false;
    }
}
// 解析函数
void parse_balance(const simdjson::dom::element& doc, const Currency& currency, UMCurrencyBalance& res);
void parse_position(const simdjson::dom::element& doc, UMSymbolPosition& res);
SpOrder parse_rtn_order(const simdjson::dom::object& obj, bool is_rest = false);
SpFundingFee parse_funding_fee(const simdjson::dom::element& doc);
void parse_pairs_info(const simdjson::dom::element& doc, const Currency& currency);

// 配置信息
inline APIConfig g_config_key_1 = {Exchange::PHEMEX, AccountType::SWAP, AddressType::NORMAL, Settlement::USDT};
inline UMExchangeConfig g_config_map = {{g_config_key_1.to_str(),
                                         {{REST_HOST, "api.phemex.com"},
                                          {WSS_PORT, "443"},
                                          {WSS_PUBLIC_HOST, "ws.phemex.com"},
                                          {WSS_PUBLIC_PATH, "/"},
                                          {WSS_PRIVATE_HOST, "ws.phemex.com"},
                                          {WSS_PRIVATE_PATH, "/"},
                                          {PAIRS_INFO_PATH, "/public/products"},
                                          {FUNDING_FEE_PATH, "/md/v3/ticker/24hr"},
                                          {BALANCE_PATH, "/g-accounts/accountPositions"},
                                          {POSITION_PATH, "/g-accounts/accountPositions"},
                                          {LEVERAGE_PATH, "/g-positions/leverage"},
                                          {QUERY_ORDER_PATH_PATH, "/api-data/g-futures/orders/by-order-id"},
                                          {CANCEL_ORDER_PATH_PATH, "/g-orders/cancel"},
                                          {PLACE_ORDER_PATH_PATH, "/g-orders"}}}};
} // namespace infra::phemex