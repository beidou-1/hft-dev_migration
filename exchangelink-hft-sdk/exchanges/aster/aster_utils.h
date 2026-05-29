#pragma once
#include "common/logger.h"
#include "common/interface.h"
#include "exchanges/exchange_utils.h"
#include "network/rest.h"
#include "exchanges/signature.h"

namespace infra::aster {
// REST请求成功代码
constexpr int64_t SUCCESS_CODE = 200;

// 缓存变量
inline UMSymbolExInfo g_pairs_info_cache;
inline Symbols g_all_symbols;
inline constexpr size_t MAX_PAIRS_PER_WS_CONNECTION = 100; // 单个连接订阅个数
// 通用函数
inline Symbol transfer_from_infra_pair(const Symbol& pair) { return to_exchange_pair(Exchange::ASTER, pair); }
inline Symbol transfer_to_infra_pair(std::string_view pair) { return to_infra_pair(Exchange::ASTER, pair); }

Errno extract_error_code(std::string_view sv);
HttpRequestBody get_request_body_with_sign(boost::beast::http::verb method, const std::string& host,
                                           const std::string& path, const std::string& query,
                                           const AccountSecret& secret);

// 解析函数
void parse_balance(const simdjson::dom::element& doc, const Currency& currency, UMCurrencyBalance& res);
void parse_position(const simdjson::dom::element& doc, UMSymbolPosition& res);
double parse_margin_ratio(const simdjson::dom::element& doc);
SpOrder parse_rtn_order(const simdjson::dom::object& obj, bool intact = false);
SpFundingFee parse_funding_fee(const simdjson::dom::element& doc);
void parse_pairs_info(const simdjson::dom::element& doc, const Currency& currency);
EcdsaSignature sign_message(const std::string& request_str, const AccountSecret& secret);
inline double get_denomination_value(const Symbol& pair) {
    auto it = g_pairs_info_cache.find(pair);
    if (it != g_pairs_info_cache.end() && it->second != nullptr) {
        return it->second->denomination_value;
    }
    return 0;
}

// 配置信息
inline APIConfig g_config_key_1 = {Exchange::ASTER, AccountType::SWAP, AddressType::NORMAL, Settlement::USDT};
inline UMExchangeConfig g_config_map = {{g_config_key_1.to_str(),
                                         {{REST_HOST, "fapi.asterdex.com"},
                                          {WSS_PORT, "443"},
                                          {WSS_PUBLIC_HOST, "fstream.asterdex.com"},
                                          {WSS_PUBLIC_PATH, "/stream"},
                                          {WSS_PRIVATE_HOST, "fstream.asterdex.com"},
                                          {WSS_PRIVATE_PATH, "/ws/"},
                                          {PAIRS_INFO_PATH, "/fapi/v3/exchangeInfo"},
                                          {FUNDING_FEE_PATH, "/fapi/v3/premiumIndex"},
                                          {BALANCE_PATH, "/fapi/v3/balance"},
                                          {POSITION_PATH, "/fapi/v3/positionRisk"},
                                          {LEVERAGE_PATH, "/fapi/v3/leverage"},
                                          {LISTEN_KEY_PATH, "/fapi/v3/listenKey"},
                                          {ORDER_PATH_PATH, "/fapi/v3/order"}}}};
} // namespace infra::aster