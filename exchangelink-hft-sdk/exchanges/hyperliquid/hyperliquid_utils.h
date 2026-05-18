#pragma once
#include "common/json.h"
#include "common/logger.h"
#include "common/interface.h"
#include "network/rest_client.h"
#include "network/wss_client.h"

namespace infra::hyperliquid {
// REST请求成功代码
constexpr int64_t SUCCESS_CODE = 0;

// 缓存变量
inline UMSymbolExInfo g_pairs_info_cache;
inline Symbols g_all_symbols;                             // 订阅全量行情时使用
inline constexpr size_t MAX_PAIRS_PER_WS_CONNECTION = 50; // 单个连接订阅个数

// 通用函数
inline Symbol transfer_from_infra_pair(const Symbol& pair) { return to_lower_str(pair); }
inline Symbol transfer_to_infra_pair(std::string_view pair) { return to_infra_pair(Exchange::HYPERLIQUID, pair); }
inline ClientOrderId transfer_oid(const ClientOrderId& oid) { return "0x" + generate_hash_md5(oid); }
Currency get_right_currency(const Currency& currency);
Errno extract_error_code(std::string_view sv);

// 解析函数
void parse_balance(const simdjson::dom::element& doc, const Currency& currency, UMCurrencyBalance& res);
void parse_position(const simdjson::dom::element& doc, UMSymbolPosition& res);
SpOrder parse_rtn_order(const simdjson::dom::object& obj);
SpFundingFee parse_funding_fee(const simdjson::dom::element& doc, const Symbol& sym);
void parse_pairs_info(const simdjson::dom::element& doc, const Currency& currency);

// 签名
void sign_action(int64_t nonce, const std::string& action, const AccountSecret& secret, EcdsaSignature& sign);

// 配置信息
inline APIConfig g_config_key_1 = {Exchange::HYPERLIQUID, AccountType::SWAP, AddressType::NORMAL, Settlement::USDT};
inline UMExchangeConfig g_config_map = {{g_config_key_1.to_str(),
                                         {{REST_HOST, "api.hyperliquid.xyz"},
                                          {WSS_PORT, "443"},
                                          {WSS_PUBLIC_HOST, "api.hyperliquid.xyz"},
                                          {WSS_PUBLIC_PATH, "/ws"},
                                          {WSS_PRIVATE_HOST, "api.hyperliquid.xyz"},
                                          {WSS_PRIVATE_PATH, "/ws"},
                                          {PAIRS_INFO_PATH, "/info"},
                                          {FUNDING_FEE_PATH, "/info"},
                                          {BALANCE_PATH, "/info"},
                                          {POSITION_PATH, "/info"},
                                          {LEVERAGE_PATH, "/exchange"},
                                          {MARGIN_MODE_PATH, "/exchange"},
                                          {POSITION_MODE_PATH, ""},
                                          {QUERY_ORDER_PATH_PATH, "/info"},
                                          {PLACE_ORDER_PATH_PATH, "/exchange"},
                                          {CANCEL_ORDER_PATH_PATH, "/exchange"}}}};
} // namespace infra::hyperliquid