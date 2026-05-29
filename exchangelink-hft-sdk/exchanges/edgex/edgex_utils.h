#pragma once
#include "common/logger.h"
#include "common/interface.h"
#include "exchanges/exchange_utils.h"
#include "network/rest.h"
#include "exchanges/signature.h"
#include <openssl/sha.h>
#include <boost/multiprecision/cpp_int.hpp>
using namespace boost::multiprecision;

namespace infra::edgex {

struct starkInfo {
    cpp_int starkAssetId;
    cpp_int starkResolution;
    double takerFee;
};

// REST请求成功代码
constexpr std::string_view SUCCESS_CODE = "SUCCESS";

// 缓存变量
inline uint32_t nonce{0};
inline UMSymbolExInfo g_pairs_info_cache;
inline Symbols g_all_symbols;
inline std::unordered_map<std::string, Symbol> coinid_to_currency_map;
inline std::unordered_map<Symbol, std::shared_ptr<starkInfo>> g_stark_info_cache;
inline constexpr size_t MAX_PAIRS_PER_WS_CONNECTION = 65; // 单个连接订阅个数

inline std::string g_account_id;

// 通用函数
inline Symbol transfer_from_infra_pair(const Symbol& pair) {
    Symbol raw = to_exchange_pair(Exchange::EDGEX, pair);
    raw.pop_back(); // xxx-usdt -> XXX-USD
    return raw;
}
inline Symbol transfer_to_infra_pair(std::string_view pair) {
    Symbol raw = to_infra_pair(Exchange::EDGEX, pair);
    raw += "t"; // XXX-USD -> xxx-usdt
    return raw;
}

inline uint32_t get_nonce() noexcept { return ++nonce; }
inline bool starts_with_0x(const std::string& s) {
    return s.size() >= 2 && (s.substr(0, 2) == "0x" || s.substr(0, 2) == "0X");
}

bool get_contract_id(const Symbol& pair, std::string& contract_id);
bool get_symbol_by_contract_id(const std::string& contract_id, Symbol& pair);

Errno extract_error_code(std::string_view sv);

std::string calc_limit_order_hash(const cpp_int& synthetic_asset_id, const cpp_int& collateral_asset_id,
                                  const cpp_int& fee_asset_id, bool is_buy, const cpp_int& amount_synthetic,
                                  const cpp_int& amount_collateral, const cpp_int& amount_fee, uint32_t nonce,
                                  const uint64_t& account_id, uint64_t expire_time, const std::string& private_key_hex);

std::string sign_edgex_params(const std::string& message, const std::string& private_key_hex,
                              const std::string& pubkey65);

HttpRequestBody get_request_body_with_sign(boost::beast::http::verb method, const std::string& host,
                                           const std::string& path, const std::string& query,
                                           const AccountSecret& secret, const std::string& payload = "");

// 解析函数
void parse_balance(const simdjson::dom::element& doc, const Currency& currency, UMCurrencyBalance& res);
void parse_position(const simdjson::dom::element& doc, UMSymbolPosition& res);
double parse_margin_ratio(const simdjson::dom::element& doc);
SpOrder parse_rtn_order(const simdjson::dom::object& obj);
SpFundingFee parse_funding_fee(const simdjson::dom::element& doc);
void parse_pairs_info(const simdjson::dom::element& doc, const Currency& currency);

// 配置信息
inline APIConfig g_config_key_1 = {Exchange::EDGEX, AccountType::SWAP, AddressType::NORMAL, Settlement::USDT};
inline UMExchangeConfig g_config_map = {{g_config_key_1.to_str(),
                                         {{REST_HOST, "pro.edgex.exchange"},
                                          {WSS_PORT, "443"},
                                          {WSS_PUBLIC_HOST, "quote.edgex.exchange"},
                                          {WSS_PUBLIC_PATH, "/api/v1/public/ws"},
                                          {WSS_PRIVATE_HOST, "quote.edgex.exchange"},
                                          {WSS_PRIVATE_PATH, "/api/v1/private/ws"},
                                          {PAIRS_INFO_PATH, "/api/v1/public/meta/getMetaData"},
                                          {FUNDING_FEE_PATH, "/api/v1/public/quote/getTicker"},
                                          {BALANCE_PATH, "/api/v1/private/account/getAccountAsset"},
                                          {POSITION_PATH, "/api/v1/private/account/getPositionByContractId"},
                                          {LEVERAGE_PATH, ""},
                                          {QUERY_ORDER_PATH_PATH, "/api/v1/private/order/getOrderById"},
                                          {PLACE_ORDER_PATH_PATH, "/api/v1/private/order/createOrder"},
                                          {CANCEL_ORDER_PATH_PATH, "/api/v1/private/order/cancelOrderById"}}}};
} // namespace infra::edgex