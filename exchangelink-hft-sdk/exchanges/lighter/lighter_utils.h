#pragma once
#include "common/json.h"
#include "common/logger.h"
#include "common/interface.h"
#include "network/rest_client.h"
#include "network/wss_client.h"
#include "lighterSDK/lighter-signer-linux-amd64.h"

namespace infra::lighter {
// REST请求成功代码
constexpr int64_t LIGHTER_SUCCESS_CODE = 200;
constexpr int64_t INVALID_MARKET_ID = -100;

// 缓存变量
inline UMSymbolExInfo g_pairs_info_cache;
inline std::unordered_map<int64_t, Symbol> g_market_id_to_symbol;
inline Symbols g_all_symbols;                             // 订阅全量行情时使用
inline constexpr size_t MAX_PAIRS_PER_WS_CONNECTION = 50; // 单个连接订阅个数

inline int g_key_index;
inline std::string g_api_token;
inline long long int g_account_index;

// 初始化全局变量，以及签名器
bool init_lighter_signer(AccountSecret& sec);

// 通用函数
inline Symbol transfer_from_infra_pair(const Symbol& pair) { return to_lower_str(pair); }
inline Symbol transfer_to_infra_pair(std::string_view pair) { return to_infra_pair(Exchange::LIGHTER, pair); }

inline SpExPairInfo get_pair_info(const Symbol& pair) {
    auto it = g_pairs_info_cache.find(to_lower_str(pair));
    return (it != g_pairs_info_cache.end()) ? it->second : nullptr;
}

bool check_client_id(const ClientOrderId& oid);
void conj_orderbook_sides_with_field(const simdjson::dom::array& data,
                                     std::list<std::pair<infra::bfloat, infra::bfloat>>& levels);

Errno extract_error_code(std::string_view sv);
HttpRequestBody get_request_body_with_tx(const std::string& host, int txType, const std::string& txInfo);

// 解析函数
void parse_balance(const simdjson::dom::element& doc, const Currency& currency, UMCurrencyBalance& res);
void parse_position(const simdjson::dom::element& doc, UMSymbolPosition& res);
SpOrder parse_rtn_order(const simdjson::dom::object& obj);
SpOrder parse_tx_order(const simdjson::dom::object& obj);
SpFundingFee parse_funding_fee(const simdjson::dom::element& doc, const Symbol& symbol);
void parse_pairs_info(const simdjson::dom::element& doc);

// nonce管理类
class NonceManager {
public:
    NonceManager() = default;
    ~NonceManager() = default;

    void update();
    long long int get(HttpClient& client);
    long long int peek(HttpClient& client);

private:
    long long int nonce_{0};
};

inline NonceManager g_nonce_manager;

// 配置信息
inline APIConfig g_config_key_1 = {Exchange::LIGHTER, AccountType::SWAP, AddressType::NORMAL, Settlement::USDT};
inline UMExchangeConfig g_config_map = {{g_config_key_1.to_str(),
                                         {{REST_HOST, "mainnet.zklighter.elliot.ai"},
                                          {WSS_PORT, "443"},
                                          {WSS_PUBLIC_HOST, "mainnet.zklighter.elliot.ai"},
                                          {WSS_PUBLIC_PATH, "/stream"},
                                          {WSS_PRIVATE_HOST, "mainnet.zklighter.elliot.ai"},
                                          {WSS_PRIVATE_PATH, "/stream"},
                                          {PAIRS_INFO_PATH, "/api/v1/orderBooks"},
                                          {FUNDING_FEE_PATH, "/api/v1/funding-rates"},
                                          {BALANCE_PATH, "/api/v1/account"},
                                          {POSITION_PATH, "/api/v1/account"},
                                          {LEVERAGE_PATH, "/api/v1/sendTx"},
                                          {NEXT_NONCE_PATH, "/api/v1/nextNonce"},
                                          {ACCOUNT_INDEX_PATH, "/api/v1/accountsByL1Address"},
                                          {CREATE_TOKEN_PATH, "/api/v1/tokens/create"},
                                          {QUERY_ORDER_PATH_PATH, "/api/v1/tx"},
                                          {PLACE_ORDER_PATH_PATH, "/api/v1/sendTx"},
                                          {CANCEL_ORDER_PATH_PATH, "/api/v1/sendTx"}}}};

} // namespace infra::lighter
