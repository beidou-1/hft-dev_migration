#pragma once
#include "common/logger.h"
#include "common/interface.h"
#include "exchanges/exchange_utils.h"
#include "network/rest.h"

namespace infra::gate {
inline Symbols g_all_symbols;
inline UMSymbolExInfo g_pairs_info_cache;
constexpr int64_t GATE_SUCCESS_CODE = 0;
inline bool g_use_sbe = true;

// 1 byte alignment
#pragma pack(push, 1)
struct SbeBookTicker {
    int64_t time;
    int8_t event;
    int64_t engine_time;
    int64_t update_id;
    int8_t price_exponent;
    int8_t size_exponent;
    int64_t ask_mant_price;
    int64_t ask_mant_size;
    int64_t bid_mant_price;
    int64_t bid_mant_size;
};
#pragma pack(pop)

// 回调处理模板
template <typename ParseFn, typename FailFn>
void handle_rest_response(HttpResponseBody& res, const std::string& msg, const char* func_name, ParseFn&& on_success,
                          FailFn&& on_fail) {
    do {
        if (res.result() != HTTP_STATUS_OK)
            break;
        try {
            PARSE_JSON(msg, doc);
            if (on_success(doc))
                return;
        } catch (const std::exception& ex) {
            INFRA_LOG_WARN("[gate] [{}] [exception], ex: {}", func_name, ex.what());
        }
    } while (0);
    INFRA_LOG_WARN("[gate] [{}] [fail], recv: {}", func_name, msg);
    on_fail();
}

inline Symbol transfer_from_infra_pair(const Symbol& pair) {
    static std::unordered_map<Symbol, Symbol> cache;
    auto it = cache.find(pair);
    if (it != cache.end()) [[likely]]
        return it->second;
    return cache[pair] = to_exchange_pair(Exchange::GATE, pair);
}

inline Symbol transfer_to_infra_pair(std::string_view pair) {
    static std::unordered_map<std::string, Symbol> cache;
    auto it = cache.find(std::string(pair));
    if (it != cache.end()) [[likely]]
        return it->second;
    return cache[std::string(pair)] = to_infra_pair(Exchange::GATE, pair);
}

inline double double_abs(double x) { return (x >= double(0)) ? x : -x; }

Currency get_right_currency(const Currency& currency);
Errno extract_error_code(std::string_view sv);
HttpRequestBody get_request_body_with_sign(boost::beast::http::verb method, const std::string& host,
                                           const std::string& path, const std::string& query, const std::string& body,
                                           const AccountSecret& secret);
std::string get_websocket_sign(const std::string& channel, const std::string& event, const std::string& timestamp,
                               const AccountSecret& secret);
std::string get_ws_api_sign(const std::string& channel, const std::string& request_param, const std::string& timestamp,
                            const AccountSecret& secret);

void parse_balance(const simdjson::dom::element& doc, const Currency& currency, UMCurrencyBalance& res);
void parse_position(const simdjson::dom::element& doc, UMSymbolPosition& res);
SpOrder parse_rtn_order(const simdjson::dom::object& obj, std::string_view channel);
SpFundingFee parse_funding_fee(const simdjson::dom::element& doc, const Symbol& pair);
void parse_pairs_info(const simdjson::dom::element& doc, const Currency& currency);
double parse_margin_ratio(const simdjson::dom::element& doc);

// 配置信息
inline APIConfig g_config_key_1 = {Exchange::GATE, AccountType::SWAP, AddressType::NORMAL, Settlement::USDT};
inline UMExchangeConfig g_config_map = {{g_config_key_1.to_str(),
                                         {
                                             {REST_HOST, "api.gateio.ws"},
                                             {WSS_PORT, "443"},
                                             {WSS_PUBLIC_HOST, "fx-ws.gateio.ws"},
                                             {WSS_PUBLIC_PATH, "/v4/ws/usdt"},
                                             {WSS_PRIVATE_HOST, "fx-ws.gateio.ws"},
                                             {WSS_PRIVATE_PATH, "/v4/ws/usdt"},
                                             {WSS_TRADE_HOST, "fx-ws.gateio.ws"},
                                             {WSS_TRADE_PATH, "/v4/ws/usdt"},
                                             {PAIRS_INFO_PATH, "/api/v4/futures/usdt/contracts"},
                                             {FUNDING_FEE_PATH, "/api/v4/futures/usdt/funding_rate"},
                                             {BALANCE_PATH, "/api/v4/futures"},
                                             {POSITION_PATH, "/api/v4/futures/usdt/positions"},
                                             {LEVERAGE_PATH, "/api/v4/futures/usdt/positions"},
                                             {ORDER_PATH_PATH, "/api/v4/futures/usdt/orders"},
                                         }}};
} // namespace infra::gate
