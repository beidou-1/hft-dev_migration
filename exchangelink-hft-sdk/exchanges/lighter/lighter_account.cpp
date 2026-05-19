#include "lighter_account.h"
using namespace infra::lighter;

namespace infra {
bool LighterAccount::initialize() {
    auto& info = g_config_map[base_config_.to_str()];
    if (info.empty()) {
        INFRA_LOG_WARN("[lighter] [initialize] [fail], msg: {} {} {} not implemented",
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

void LighterAccount::get_balance(const Currency& currency, BalanceCallback cb) {
    if (balance_path_.empty()) {
        cb(Errno::NotImplemented, {});
        return;
    }

    std::string query = "by=index&value=" + std::to_string(g_account_index);
    auto req = get_request_body(rest_host_, balance_path_, query);
    rest_.send(req, [this, currency, cb](HttpResponseBody& res) {
        std::string msg = boost::beast::buffers_to_string(res.body().data());
        do {
            if (res.result() != HTTP_STATUS_OK) {
                break;
            }
            try {
                PARSE_JSON(msg, doc);
                if (doc["code"].get_int64() != LIGHTER_SUCCESS_CODE) {
                    break;
                }
                INFRA_LOG_INFO("[lighter] [get_balance] [success], recv: {}", msg);
                UMCurrencyBalance assets;
                parse_balance(doc, currency, assets);
                cb(Errno::Ok, assets);
                return;
            } catch (const std::exception& ex) {
                INFRA_LOG_WARN("[lighter] [get_balance] [exception], msg: {}", ex.what());
            }
        } while (0);
        INFRA_LOG_WARN("[lighter] [get_balance] [fail], recv: {}", msg);
        cb(extract_error_code(msg), {});
    });
}

void LighterAccount::get_position(const Symbol& symbol, PositionCallback cb) {
    if (position_path_.empty()) {
        cb(Errno::NotImplemented, {});
        return;
    }

    std::string query = "by=index&value=" + std::to_string(g_account_index);
    auto req = get_request_body(rest_host_, position_path_, query);
    rest_.send(req, [this, cb](HttpResponseBody& res) {
        std::string msg = boost::beast::buffers_to_string(res.body().data());
        do {
            if (res.result() != HTTP_STATUS_OK) {
                break;
            }
            try {
                PARSE_JSON(msg, doc);
                if (doc["code"].get_int64() != LIGHTER_SUCCESS_CODE) {
                    break;
                }
                INFRA_LOG_INFO("[lighter] [get_position] [success], recv: {}", msg);
                UMSymbolPosition positions;
                parse_position(doc, positions);
                cb(Errno::Ok, positions);
                return;
            } catch (const std::exception& ex) {
                INFRA_LOG_WARN("[lighter] [get_position] [exception], msg: {}", ex.what());
            }
        } while (0);
        INFRA_LOG_WARN("[lighter] [get_position] [fail], recv: {}", msg);
        cb(extract_error_code(msg), {});
    });
}

void LighterAccount::set_leverage(const Symbol& symbol, unsigned int leverage, MarginMode mode, LeverageCallback cb) {
    if (leverage_path_.empty() || symbol.empty() || leverage == 0) {
        cb(Errno::InvalidParams);
        return;
    }

    Symbol pair = transfer_from_infra_pair(symbol);
    auto it = g_pairs_info_cache.find(pair);
    if (it == g_pairs_info_cache.end()) {
        INFRA_LOG_WARN("[lighter] [set_leverage] [fail], msg: not found {} in cache", pair);
        cb(Errno::InvalidParams);
        return;
    }

    int cMarketIndex = std::stoi(it->second->alias);
    int cImf = int(10000 / leverage);
    int cMode = (mode == MarginMode::CROSS) ? 0 : 1;
    long long int cNonce = g_nonce_manager.get(rest_);
    SignedTxResponse res = SignUpdateLeverage(cMarketIndex, cImf, cMode, cNonce, g_key_index, g_account_index);
    if (res.err != nullptr) {
        INFRA_LOG_WARN("[lighter] [set_leverage] [fail], err: {}", res.err);
        cb(Errno::UnknownError);
        return;
    }

    g_nonce_manager.update();
    auto req = get_request_body_with_tx(rest_host_, res.txType, res.txInfo);
    INFRA_LOG_INFO("[lighter] [set_leverage], txType:{}, txHash:{}, txInfo:{}", res.txType, res.txHash, res.txInfo);
    rest_.send(req, [this, cb](HttpResponseBody& res) {
        std::string msg = boost::beast::buffers_to_string(res.body().data());
        do {
            if (res.result() != HTTP_STATUS_OK)
                break;
            try {
                PARSE_JSON(msg, doc);
                if (doc["code"].get_int64() != LIGHTER_SUCCESS_CODE) {
                    if (doc["code"].get_int64() == 21104) {
                        g_nonce_manager.peek(rest_);
                    }
                    break;
                }
                INFRA_LOG_INFO("[lighter] [set_leverage] [success], recv: {}", msg);
                cb(Errno::Ok);
                return;
            } catch (const std::exception& ex) {
                INFRA_LOG_WARN("[lighter] [set_leverage] [exception], msg: {}", ex.what());
            }
        } while (0);
        INFRA_LOG_WARN("[lighter] [set_leverage] [fail], recv: {}", msg);
        cb(extract_error_code(msg));
    });
}

void LighterAccount::get_margin_ratio(MarginRatioCallback cb) {

    std::string query = "by=index&value=" + std::to_string(g_account_index);
    auto req = get_request_body(rest_host_, balance_path_, query);
    rest_.send(req, [this, cb](HttpResponseBody& res) {
        std::string msg = boost::beast::buffers_to_string(res.body().data());
        do {
            if (res.result() != HTTP_STATUS_OK)
                break;
            try {
                PARSE_JSON(msg, doc);
                if (doc["code"].get_int64() != LIGHTER_SUCCESS_CODE)
                    break;
                INFRA_LOG_INFO("[lighter] [get_margin_ratio] [success]");
                cb(Errno::Ok, parse_margin_ratio(doc));
                return;
            } catch (const std::exception& ex) {
                INFRA_LOG_WARN("[lighter] [get_margin_ratio] [exception], msg: {}", ex.what());
            }
        } while (0);
        INFRA_LOG_WARN("[lighter] [get_margin_ratio] [fail], recv: {}", msg);
        cb(extract_error_code(msg), 0);
    });
}

} // namespace infra