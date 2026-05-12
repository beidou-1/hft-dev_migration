#include "bybit_account.h"
using namespace infra::bybit;

namespace infra {
bool BybitAccount::initialize() {
    auto& info = g_config_map[base_config_.to_str()];
    if (info.empty()) {
        INFRA_LOG_WARN("[bybit] [initialize] [fail], msg: {} {} {} not implemented",
                       to_string(base_config_.account_type), to_string(base_config_.address_type),
                       to_string(base_config_.settle_unit));
        return false;
    }

    rest_host_ = info[REST_HOST];
    category_ = "linear";
    balance_path_ = info[BALANCE_PATH];
    position_path_ = info[POSITION_PATH];
    leverage_path_ = info[LEVERAGE_PATH];
    position_mode_path_ = info[POSITION_MODE_PATH];
    return true;
}

UMCurrencyBalance BybitAccount::get_balance(const Currency& currency) {
    if (balance_path_.empty()) {
        return {};
    }

    std::string query{};
    query.append("accountType=UNIFIED");
    if (!currency.empty()) {
        query.append("&coin=").append(get_right_currency(currency));
    }
    auto req = get_request_body_with_sign(HTTP_GET, rest_host_, balance_path_, query, account_secret_);
    boost::beast::error_code ec;
    std::string response = rest_.sync_send(req, ec);
    do {
        if (ec) {
            break;
        }
        try {
            PARSE_JSON(response, doc);
            if (doc["retCode"].get_int64() != BYBIT_SUCCESS_CODE) {
                break;
            }
            INFRA_LOG_INFO("[bybit] [get_balance] [success], recv: {}", response);
            UMCurrencyBalance assets;
            parse_balance(doc, currency, assets);
            return assets;
        } catch (const std::exception& ex) {
            INFRA_LOG_WARN("[bybit] [get_balance] [exception], msg: {}", ex.what());
        }
    } while (0);
    INFRA_LOG_WARN("[bybit] [get_balance] [fail], recv: {}", response);
    return {};
}

UMSymbolPosition BybitAccount::get_position(const Symbol& symbol) {
    if (position_path_.empty()) {
        return {};
    }

    std::string query{};
    query.append("category=").append(category_);
    query.append("&settleCoin=").append("USDT");
    if (!symbol.empty()) {
        query.append("&symbol=").append(transfer_from_infra_pair(symbol));
    }
    auto req = get_request_body_with_sign(HTTP_GET, rest_host_, position_path_, query, account_secret_);
    boost::beast::error_code ec;
    std::string response = rest_.sync_send(req, ec);
    do {
        if (ec) {
            break;
        }
        try {
            PARSE_JSON(response, doc);
            if (doc["retCode"].get_int64() != BYBIT_SUCCESS_CODE) {
                break;
            }
            INFRA_LOG_INFO("[bybit] [get_position] [success], recv: {}", response);
            UMSymbolPosition positions;
            parse_position(doc, positions);
            return positions;
        } catch (const std::exception& ex) {
            INFRA_LOG_WARN("[bybit] [get_position] [exception], msg: {}", ex.what());
        }
    } while (0);
    INFRA_LOG_WARN("[bybit] [get_position] [fail], recv: {}", response);
    return {};
}

void BybitAccount::get_balance(const Currency& currency, BalanceCallback cb) {
    if (balance_path_.empty()) {
        cb(Errno::NotImplemented, {});
        return;
    }

    std::string query{};
    query.append("accountType=UNIFIED");
    if (!currency.empty()) {
        query.append("&coin=").append(get_right_currency(currency));
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
                if (doc["retCode"].get_int64() != BYBIT_SUCCESS_CODE) {
                    break;
                }
                INFRA_LOG_INFO("[bybit] [get_balance] [success], recv: {}", response);
                UMCurrencyBalance assets;
                parse_balance(doc, currency, assets);
                cb(Errno::Ok, assets);
                return;
            } catch (const std::exception& ex) {
                INFRA_LOG_WARN("[bybit] [get_balance] [exception], msg: {}", ex.what());
            }
        } while (0);
        INFRA_LOG_WARN("[bybit] [get_balance] [fail], recv: {}", response);
        cb(extract_error_msg(response), {});
    });
}

void BybitAccount::get_position(const Symbol& symbol, PositionCallback cb) {
    if (position_path_.empty()) {
        cb(Errno::NotImplemented, {});
        return;
    }

    std::string query{};
    query.append("category=").append(category_);
    query.append("&settleCoin=").append("USDT");
    if (!symbol.empty()) {
        query.append("&symbol=").append(transfer_from_infra_pair(symbol));
    }

    auto req = get_request_body_with_sign(HTTP_GET, rest_host_, position_path_, query, account_secret_);
    rest_.send(req, [this, cb](HttpResponseBody& res) {
        std::string response = boost::beast::buffers_to_string(res.body().data());
        do {
            if (res.result() != HTTP_STATUS_OK) {
                break;
            }
            try {
                PARSE_JSON(response, doc);
                if (doc["retCode"].get_int64() != BYBIT_SUCCESS_CODE) {
                    break;
                }
                INFRA_LOG_INFO("[bybit] [get_position] [success], recv: {}", response);
                UMSymbolPosition positions;
                parse_position(doc, positions);
                cb(Errno::Ok, positions);
                return;
            } catch (const std::exception& ex) {
                INFRA_LOG_WARN("[bybit] [get_position] [exception], msg: {}", ex.what());
            }
        } while (0);
        INFRA_LOG_WARN("[bybit] [get_position] [fail], recv: {}", response);
        cb(extract_error_msg(response), {});
    });
}

bool BybitAccount::set_leverage(const Symbol& symbol, unsigned int leverage, MarginMode mode) {
    if (leverage_path_.empty() || symbol.empty() || leverage == 0) {
        return false;
    }

    std::string request_body =
        fmt::format(R"({{"category":"{}","symbol":"{}","buyLeverage":"{}","sellLeverage":"{}"}})", category_,
                    transfer_from_infra_pair(symbol), leverage, leverage);
    auto req = get_request_body_with_sign(HTTP_POST, rest_host_, leverage_path_, request_body, account_secret_);
    boost::beast::error_code ec;
    std::string response = rest_.sync_send(req, ec);
    do {
        if (ec) {
            break;
        }
        try {
            PARSE_JSON(response, doc);
            if (doc["retCode"].get_int64() == BYBIT_SUCCESS_CODE || doc["retCode"].get_int64() == 110043) {
                INFRA_LOG_INFO("[bybit] [set_leverage] [success], msg: set leverage {} for symbol {}", leverage,
                               symbol);
                return true;
            }
        } catch (const std::exception& ex) {
            INFRA_LOG_WARN("[bybit] [set_leverage] [exception], exception: {}", ex.what());
            return false;
        }
    } while (0);
    INFRA_LOG_WARN("[bybit] [set_leverage] [fail], response: {}", response);
    return false;
}

bool BybitAccount::set_margin_mode(const Symbol& symbol, MarginMode mode) {
    INFRA_LOG_WARN("[bybit] [set_margin_mode] [fail], not supported");
    return false;
}

bool BybitAccount::set_position_mode(PositionMode mode) {
    if (position_mode_path_.empty()) {
        return false;
    }

    int mode_int = (mode == PositionMode::hedge_mode) ? 3 : 0;
    std::string request_body = fmt::format(R"({{"category":"{}","mode":{},"coin":"USDT"}})", category_, mode_int);
    auto req = get_request_body_with_sign(HTTP_POST, rest_host_, position_mode_path_, request_body, account_secret_);
    if (send_http_request_sync(req, "set_position_mode")) {
        g_current_position_mode = mode;
        return true;
    }
    return false;
}

bool BybitAccount::send_http_request_sync(const HttpRequestBody& req, std::string_view name) {
    boost::beast::error_code ec;
    std::string response = rest_.sync_send(req, ec);
    if (ec) {
        INFRA_LOG_WARN("[bybit] [{}] [fail], response: {}", name, response);
        return false;
    }

    try {
        PARSE_JSON(response, doc);
        if (doc["retCode"].get_int64() != BYBIT_SUCCESS_CODE) {
            INFRA_LOG_WARN("[bybit] [{}] [fail], response: {}", name, response);
            return false;
        }
    } catch (const std::exception& ex) {
        INFRA_LOG_WARN("[bybit] [{}] [exception], msg: parse error: {}, response: {}", name, ex.what(), response);
        return false;
    }
    INFRA_LOG_INFO("[bybit] [{}] [success], response: {}", name, response);
    return true;
}
} // namespace infra
