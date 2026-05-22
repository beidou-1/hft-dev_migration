#include "phemex_account.h"
using namespace infra::phemex;

namespace infra {
bool PhemexAccount::initialize() {
    auto& info = g_config_map[base_config_.to_str()];
    if (info.empty()) {
        INFRA_LOG_WARN("[phemex] [initialize] [fail], msg: {} {} {} not implemented",
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

UMCurrencyBalance PhemexAccount::get_balance(const Currency& currency) {
    if (balance_path_.empty()) {
        return {};
    }

    if (currency.empty()) {
        INFRA_LOG_WARN("[phemex] [get_balance] [fail], msg: empty currency not support");
        return {};
    }

    std::string query{};
    std::string ccy = currency;
    std::transform(ccy.begin(), ccy.end(), ccy.begin(), ::toupper);
    query.append("currency=").append(ccy);
    auto req = get_request_body_with_sign(HTTP_GET, rest_host_, balance_path_, query, account_secret_);
    boost::beast::error_code ec;
    std::string response = rest_.sync_send(req, ec);
    do {
        if (ec) {
            break;
        }
        try {
            PARSE_JSON(response, doc);
            if (doc["code"].get_int64() != SUCCESS_CODE) {
                break;
            }
            // INFRA_LOG_INFO("[phemex] [get_balance] [success], recv: {}", response);
            UMCurrencyBalance assets;
            parse_balance(doc, currency, assets);
            return assets;
        } catch (const std::exception& ex) {
            INFRA_LOG_WARN("[phemex] [get_balance] [exception], msg: {}", ex.what());
        }
    } while (0);
    INFRA_LOG_WARN("[phemex] [get_balance] [fail], recv: {}", response);
    return {};
}

UMSymbolPosition PhemexAccount::get_position(const Symbol& symbol) {
    if (position_path_.empty()) {
        return {};
    }
    std::string query{};
    query.append("currency=USDT");
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
            if (doc["code"].get_int64() != SUCCESS_CODE) {
                break;
            }
            // INFRA_LOG_INFO("[phemex] [get_position] [success], recv: {}", response);
            UMSymbolPosition positions;
            parse_position(doc, positions);
            return positions;
        } catch (const std::exception& ex) {
            INFRA_LOG_WARN("[phemex] [get_position] [exception], msg: {}", ex.what());
        }
    } while (0);
    INFRA_LOG_WARN("[phemex] [get_position] [fail], recv: {}", response);
    return {};
}

void PhemexAccount::get_balance(const Currency& currency, BalanceCallback cb) {
    if (balance_path_.empty()) {
        cb(Errno::NotImplemented, {});
        return;
    }

    if (currency.empty()) {
        INFRA_LOG_WARN("[phemex] [get_balance] [fail], msg: empty currency not support");
        cb(Errno::InvalidParams, {});
        return;
    }
    std::string query{};
    std::string ccy = currency;
    std::transform(ccy.begin(), ccy.end(), ccy.begin(), ::toupper);
    query.append("currency=").append(ccy);
    auto req = get_request_body_with_sign(HTTP_GET, rest_host_, balance_path_, query, account_secret_);
    rest_.send(req, [this, currency, cb](HttpResponseBody& res) {
        std::string response = boost::beast::buffers_to_string(res.body().data());
        do {
            if (res.result() != HTTP_STATUS_OK) {
                break;
            }
            try {
                PARSE_JSON(response, doc);
                if (doc["code"].get_int64() != SUCCESS_CODE) {
                    break;
                }
                // INFRA_LOG_INFO("[phemex] [get_balance] [success], recv: {}", response);
                UMCurrencyBalance assets;
                parse_balance(doc, currency, assets);
                cb(Errno::Ok, assets);
                return;
            } catch (const std::exception& ex) {
                INFRA_LOG_WARN("[phemex] [get_balance] [exception], msg: {}", ex.what());
            }
        } while (0);
        INFRA_LOG_WARN("[phemex] [get_balance] [fail], recv: {}", response);
        cb(extract_error_code(response), {});
    });
}

void PhemexAccount::get_position(const Symbol& symbol, PositionCallback cb) {
    if (position_path_.empty()) {
        cb(Errno::NotImplemented, {});
        return;
    }

    std::string query{};
    query.append("currency=USDT");
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
                if (doc["code"].get_int64() != SUCCESS_CODE) {
                    break;
                }
                // INFRA_LOG_INFO("[phemex] [get_position] [success], recv: {}", response);
                UMSymbolPosition positions;
                parse_position(doc, positions);
                cb(Errno::Ok, positions);
                return;
            } catch (const std::exception& ex) {
                INFRA_LOG_WARN("[phemex] [get_position] [exception], msg: {}", ex.what());
            }
        } while (0);
        INFRA_LOG_WARN("[phemex] [get_position] [fail], recv: {}", response);
        cb(extract_error_code(response), {});
    });
}

void PhemexAccount::set_leverage(const Symbol& symbol, unsigned int leverage, MarginMode mode, LeverageCallback cb) {
    if (leverage_path_.empty() || symbol.empty() || leverage == 0) {
        return false;
    }
    std::string query = "symbol="+transfer_from_infra_pair(symbol);
    if (g_current_position_mode == PositionMode::one_way_mode) {
        query.append("&leverageRr="+std::to_string(leverage));
    } else {
        query.append("&longLeverageRr="+std::to_string(leverage)+"&shortLeverageRr="+std::to_string(leverage));
    }
    auto req = get_request_body_with_sign(HTTP_PUT, rest_host_, leverage_path_, query, account_secret_);
    return send_http_request_sync(req, "set_leverage");
}

bool PhemexAccount::send_http_request_sync(const HttpRequestBody& req, std::string_view name) {
    boost::beast::error_code ec;
    std::string response = rest_.sync_send(req, ec);
    do {
        if (ec) {
            break;
        }
        try {
            PARSE_JSON(response, doc);
            if (doc["code"].get_int64() != SUCCESS_CODE) {
                break;
            }
            INFRA_LOG_INFO("[phemex] [{}] [success], recv: {}", name, response);
            return true;
        } catch (const std::exception& ex) {
            INFRA_LOG_WARN("[phemex] [{}] [exception], msg: {}", name, ex.what());
        }
    } while (0);
    INFRA_LOG_WARN("[phemex] [{}] [fail], recv: {}", name, response);
    return false;
}
} // namespace infra
