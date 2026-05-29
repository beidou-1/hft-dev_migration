#include "kucoin_account.h"
using namespace infra::kucoin;

namespace infra {
bool KucoinAccount::initialize() {
    std::string key;
    if (g_account_mode == AccountMode::CLASSIC) {
        key = "classic";
    } else {
        key = base_config_.to_str();
    }

    auto& info = g_config_map[key];
    if (info.empty()) {
        INFRA_LOG_WARN("[kucoin] [initialize] [fail], msg: {} {} {} {} not implemented",
                       to_string(base_config_.account_type), to_string(base_config_.address_type),
                       to_string(base_config_.account_mode), to_string(base_config_.settle_unit));
        return false;
    }

    rest_host_ = info[REST_HOST];
    balance_path_ = info[BALANCE_PATH];
    position_path_ = info[POSITION_PATH];
    leverage_path_ = info[LEVERAGE_PATH];
    return true;
}

void KucoinAccount::get_balance(const Currency& currency, BalanceCallback cb) {
    if (g_account_mode == AccountMode::CLASSIC) {
        get_classic_balance(currency, cb);
    } else if (g_account_mode == AccountMode::UNIFIED) {
        get_unified_balance(currency, cb);
    }
}

void KucoinAccount::get_classic_balance(const Currency& currency, BalanceCallback cb) {
    if (balance_path_.empty()) {
        cb(Errno::NotImplemented, {});
        return;
    }

    std::string query{};
    if (not currency.empty()) {
        query.append("currency=").append(get_right_currency(currency));
    }

    auto req = get_request_body_with_sign(HTTP_GET, rest_host_, balance_path_, query, account_secret_);
    rest_.send(req, [this, currency, cb](HttpResponseBody& res) {
        std::string response = boost::beast::buffers_to_string(res.body().data());
        do {
            if (res.result() != HTTP_STATUS_OK) {
                break;
            }
            try {
                PARSE_JSON(response, doc);
                if (doc["code"].get_string()->compare(KUCOIN_SUCCESS_CODE) != 0) {
                    break;
                }
                INFRA_LOG_INFO("[kucoin] [get_balance] [success], recv: {}", response);
                UMCurrencyBalance assets;
                parse_classic_balance(doc, currency, assets);
                cb(Errno::Ok, assets);
                return;
            } catch (const std::exception& ex) {
                INFRA_LOG_WARN("[kucoin] [get_balance] [exception], msg: {}", ex.what());
            }
        } while (0);
        INFRA_LOG_WARN("[kucoin] [get_balance] [fail], recv: {}", response);
        cb(extract_error_code(response), {});
    });
}

void KucoinAccount::get_unified_balance(const Currency& currency, BalanceCallback cb) {
    if (balance_path_.empty()) {
        cb(Errno::NotImplemented, {});
        return;
    }

    auto req = get_request_body_with_sign(HTTP_GET, rest_host_, balance_path_, "", account_secret_);
    rest_.send(req, [this, currency, cb](HttpResponseBody& res) {
        std::string response = boost::beast::buffers_to_string(res.body().data());
        do {
            if (res.result() != HTTP_STATUS_OK) {
                break;
            }
            try {
                PARSE_JSON(response, doc);
                if (doc["code"].get_string()->compare(KUCOIN_SUCCESS_CODE) != 0) {
                    break;
                }
                INFRA_LOG_INFO("[kucoin] [get_balance] [success], recv: {}", response);
                UMCurrencyBalance assets;
                parse_unified_balance(doc, currency, assets);
                cb(Errno::Ok, assets);
                return;
            } catch (const std::exception& ex) {
                INFRA_LOG_WARN("[kucoin] [get_balance] [exception], msg: {}", ex.what());
            }
        } while (0);
        INFRA_LOG_WARN("[kucoin] [get_balance] [fail], recv: {}", response);
        cb(extract_error_code(response), {});
    });
}

void KucoinAccount::get_position(const Symbol& symbol, PositionCallback cb) {
    if (position_path_.empty()) {
        cb(Errno::NotImplemented, {});
        return;
    }

    std::string query{};
    if (not symbol.empty()) {
        query.append("symbol=").append(transfer_from_infra_pair(symbol));
    }

    auto req = get_request_body_with_sign(HTTP_GET, rest_host_, position_path_, query, account_secret_);
    rest_.send(req, [this, symbol, cb](HttpResponseBody& res) {
        std::string response = boost::beast::buffers_to_string(res.body().data());
        do {
            if (res.result() != HTTP_STATUS_OK) {
                break;
            }
            try {
                PARSE_JSON(response, doc);
                if (doc["code"].get_string()->compare(KUCOIN_SUCCESS_CODE) != 0) {
                    break;
                }
                INFRA_LOG_INFO("[kucoin] [get_position] [success], recv: {}", response);
                UMSymbolPosition positions;
                if (g_account_mode == AccountMode::CLASSIC) {
                    parse_classic_position(doc, symbol, positions);
                } else if (g_account_mode == AccountMode::UNIFIED) {
                    parse_unified_position(doc, symbol, positions);
                }
                cb(Errno::Ok, positions);
                return;
            } catch (const std::exception& ex) {
                INFRA_LOG_WARN("[kucoin] [get_position] [exception], msg: {}", ex.what());
            }
        } while (0);
        INFRA_LOG_WARN("[kucoin] [get_position] [fail], recv: {}", response);
        cb(extract_error_code(response), {});
    });
}

void KucoinAccount::set_leverage(const Symbol& symbol, unsigned int leverage, MarginMode mode, LeverageCallback cb) {

    std::string exchange_symbol = transfer_from_infra_pair(symbol);
    std::string request_body = fmt::format(R"({{"symbol":"{}","leverage":"{}"}})", exchange_symbol, leverage);
    auto req = get_request_body_with_sign(HTTP_POST, rest_host_, leverage_path_, request_body, account_secret_);
    rest_.send(req, [this, cb](HttpResponseBody& res) {
        std::string response = boost::beast::buffers_to_string(res.body().data());
        do {
            if (res.result() != HTTP_STATUS_OK) {
                break;
            }
            try {
                PARSE_JSON(response, doc);
                if (doc["code"].get_string()->compare(KUCOIN_SUCCESS_CODE) != 0) {
                    break;
                }
                INFRA_LOG_INFO("[kucoin] [set_leverage] [success], recv: {}", response);
                cb(Errno::Ok);
                return;
            } catch (const std::exception& ex) {
                INFRA_LOG_WARN("[kucoin] [set_leverage] [exception], msg: {}", ex.what());
            }
        } while (0);
        INFRA_LOG_WARN("[kucoin] [set_leverage] [fail], recv: {}", response);
        cb(extract_error_code(response));
    });
}

void KucoinAccount::get_margin_ratio(MarginRatioCallback cb) {
    if (balance_path_.empty()) {
        cb(Errno::NotImplemented, 0);
        return;
    }

    auto req = get_request_body_with_sign(HTTP_GET, rest_host_, balance_path_, "", account_secret_);
    rest_.send(req, [this, cb](HttpResponseBody& res) {
        std::string msg = boost::beast::buffers_to_string(res.body().data());
        do {
            if (res.result() != HTTP_STATUS_OK)
                break;
            try {
                PARSE_JSON(msg, doc);
                if (doc["code"].get_string()->compare(KUCOIN_SUCCESS_CODE) != 0)
                    break;
                INFRA_LOG_INFO("[kucoin] [get_margin_ratio] [success]");
                if (g_account_mode == AccountMode::CLASSIC) {
                    cb(Errno::Ok, parse_classic_margin_ratio(doc));
                } else if (g_account_mode == AccountMode::UNIFIED) {
                    cb(Errno::Ok, parse_unified_margin_ratio(doc));
                }
                return;
            } catch (const std::exception& ex) {
                INFRA_LOG_WARN("[kucoin] [get_margin_ratio] [exception], exception: {}", ex.what());
            }
        } while (0);
        INFRA_LOG_WARN("[kucoin] [get_margin_ratio] [fail], response: {}", msg);
        cb(extract_error_code(msg), 0);
    });
}

} // namespace infra
