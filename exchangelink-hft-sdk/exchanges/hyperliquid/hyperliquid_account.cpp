#include "hyperliquid_account.h"
using namespace infra::hyperliquid;

namespace infra {
bool HyperliquidAccount::initialize() {
    auto& info = g_config_map[base_config_.to_str()];
    if (info.empty()) {
        INFRA_LOG_WARN("[hyperliquid] [initialize] [fail], msg: {} {} {} not implemented",
                       to_string(base_config_.account_type), to_string(base_config_.address_type),
                       to_string(base_config_.settle_unit));
        return false;
    }

    rest_host_ = info[REST_HOST];
    balance_path_ = info[BALANCE_PATH];
    position_path_ = info[POSITION_PATH];
    leverage_path_ = info[LEVERAGE_PATH];
    return true;
}

void HyperliquidAccount::get_balance(const Currency& currency, BalanceCallback cb) {
    if (balance_path_.empty()) {
        cb(Errno::NotImplemented, {});
        return;
    }

    std::string query = fmt::format(R"({{"type":"clearinghouseState","user":"{}"}})", account_secret_.wallet_address);
    auto req = get_request_body_by_post(rest_host_, balance_path_, query);
    rest_.send(req, [this, currency, cb](HttpResponseBody& res) {
        std::string response = boost::beast::buffers_to_string(res.body().data());

        do {
            if (res.result() != HTTP_STATUS_OK) {
                break;
            }
            try {
                PARSE_JSON(response, doc);
                // if (doc["code"].get_int64() != SUCCESS_CODE) {
                //     break;
                // }
                INFRA_LOG_INFO("[hyperliquid] [get_balance] [success], recv: {}", response);
                UMCurrencyBalance assets;
                parse_balance(doc, currency, assets);
                cb(Errno::Ok, assets);
                return;
            } catch (const std::exception& ex) {
                INFRA_LOG_WARN("[hyperliquid] [get_balance] [exception], msg: {}", ex.what());
            }
        } while (0);
        INFRA_LOG_WARN("[hyperliquid] [get_balance] [fail], recv: {}", response);
        cb(extract_error_code(response), {});
    });
}

void HyperliquidAccount::get_position(const Symbol& symbol, PositionCallback cb) {
    if (position_path_.empty()) {
        cb(Errno::NotImplemented, {});
        return;
    }

    std::string query = fmt::format(R"({{"type":"clearinghouseState","user":"{}"}})", account_secret_.wallet_address);
    auto req = get_request_body_by_post(rest_host_, position_path_, query);
    rest_.send(req, [this, cb](HttpResponseBody& res) {
        std::string response = boost::beast::buffers_to_string(res.body().data());
        do {
            if (res.result() != HTTP_STATUS_OK) {
                break;
            }
            try {
                PARSE_JSON(response, doc);
                // if (doc["code"].get_int64() != SUCCESS_CODE) {
                //     break;
                // }
                INFRA_LOG_INFO("[hyperliquid] [get_position] [success], recv: {}", response);
                UMSymbolPosition positions;
                parse_position(doc, positions);
                cb(Errno::Ok, positions);
                return;
            } catch (const std::exception& ex) {
                INFRA_LOG_WARN("[hyperliquid] [get_position] [exception], msg: {}", ex.what());
            }
        } while (0);
        INFRA_LOG_WARN("[hyperliquid] [get_position] [fail], recv: {}", response);
        cb(extract_error_code(response), {});
    });
}

void HyperliquidAccount::set_leverage(const Symbol& symbol, unsigned int leverage, MarginMode mode, LeverageCallback cb) {
    if (leverage_path_.empty() || symbol.empty() || leverage == 0) {
        return false;
    }

    Symbol pair = transfer_from_infra_pair(symbol);
    auto it = g_pairs_info_cache.find(pair);
    if (it == g_pairs_info_cache.end()) {
        INFRA_LOG_WARN("[hyperliquid] [set_leverage] [fail], msg: not found {} in cache", symbol);
        return false;
    }

    SpExPairInfo pair_info = it->second;
    int asset_id = std::stoi(pair_info->alias);

    bool isCross = (mode == MarginMode::CROSS) ? true : false;
    std::string action_str = fmt::format(R"({{"type":"updateLeverage","asset":{},"isCross":{},"leverage":{}}})",
                                         asset_id, isCross, leverage);
    int64_t nonce = time_get_now_milli();

    EcdsaSignature sign;
    sign_action(nonce, action_str, account_secret_, sign);

    std::string payload = fmt::format(R"({{"action":{},"nonce":{},"signature":{{"r":"{}","s":"{}","v":{}}}}})",
                                      action_str, nonce, sign.r_hex, sign.s_hex, sign.v);
    INFRA_LOG_INFO("[hyperliquid] [set_leverage], send: {}", payload);
    auto req = get_request_body_by_post(rest_host_, leverage_path_, payload);
    rest_.send(req, [this, cb, leverage, symbol](HttpResponseBody& res) {
        std::string msg = boost::beast::buffers_to_string(res.body().data());
        do {
            if (res.result() != HTTP_STATUS_OK)
                break;
            try {
                PARSE_JSON(msg, doc);
                if (doc["status"].error() == simdjson::SUCCESS && doc["status"].get_string().value() == "ok") {
                    INFRA_LOG_INFO("[hyperliquid] [set_leverage] [success], msg: set leverage {} for symbol {}", leverage,
                                   symbol);
                    cb(Errno::Ok);
                    return;
                }
            } catch (const std::exception& ex) {
                INFRA_LOG_WARN("[hyperliquid] [set_leverage] [exception], exception: {}", ex.what());
            }
        } while (0);
        INFRA_LOG_WARN("[hyperliquid] [set_leverage] [fail], response: {}", msg);
        cb(extract_error_msg(msg));
    });
}

} // namespace infra
