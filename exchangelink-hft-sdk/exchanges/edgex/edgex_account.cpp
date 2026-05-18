#include "edgex_account.h"
using namespace infra::edgex;

namespace infra {
bool EdgexAccount::initialize() {
    auto& info = g_config_map[base_config_.to_str()];
    if (info.empty()) {
        INFRA_LOG_WARN("[edgex] [initialize] [fail], msg: {} {} {} not implemented",
                       to_string(base_config_.account_type), to_string(base_config_.address_type),
                       to_string(base_config_.settle_unit));
        return false;
    }

    rest_host_ = info[REST_HOST];
    balance_path_ = info[BALANCE_PATH];
    position_path_ = info[POSITION_PATH];
    leverage_path_ = info[LEVERAGE_PATH];
    margin_mode_path_ = info[MARGIN_MODE_PATH];
    position_mode_path_ = info[POSITION_MODE_PATH];
    return true;
}

UMCurrencyBalance EdgexAccount::get_balance(const Currency& currency) {
    if (balance_path_.empty()) {
        return {};
    }

    std::string query{};
    query.append("accountId=").append(g_account_id);
    auto req = get_request_body_with_sign(HTTP_GET, rest_host_, balance_path_, query, account_secret_);
    boost::beast::error_code ec;
    std::string response = rest_.sync_send(req, ec);
    do {
        if (ec) {
            break;
        }
        try {
            PARSE_JSON(response, doc);
            if (doc["code"].error() != simdjson::SUCCESS) {
                break;
            }
            std::string_view code_str = doc["code"];
            if (code_str != SUCCESS_CODE) {
                break;
            }

            INFRA_LOG_INFO("[edgex] [get_balance] [success]");
            UMCurrencyBalance assets;
            parse_balance(doc, currency, assets);
            return assets;
        } catch (const std::exception& ex) {
            INFRA_LOG_WARN("[edgex] [get_balance] [exception], msg: {}", ex.what());
        }
    } while (0);
    INFRA_LOG_WARN("[edgex] [get_balance] [fail], recv: {}", response);
    return {};
}

UMSymbolPosition EdgexAccount::get_position(const Symbol& symbol) {
    if (position_path_.empty()) {
        return {};
    }

    if (symbol.empty()) {
        INFRA_LOG_WARN("[edgex] [get_position] [fail], msg: symbol is empty");
        return {};
    }

    std::string query{};
    query.append("accountId=").append(g_account_id);
    std::string contractId;
    if (!get_contract_id(symbol, contractId)) {
        INFRA_LOG_WARN("[edgex] [get_position] [fail], msg: get contract id failed for pair {}", symbol);
        return {};
    }
    query.append("&contractIdList=").append(contractId);
    auto req = get_request_body_with_sign(HTTP_GET, rest_host_, position_path_, query, account_secret_);
    boost::beast::error_code ec;
    std::string response = rest_.sync_send(req, ec);
    do {
        if (ec) {
            break;
        }
        try {
            PARSE_JSON(response, doc);
            if (doc["code"].error() != simdjson::SUCCESS) {
                break;
            }
            std::string_view code_str = doc["code"];
            if (code_str != SUCCESS_CODE) {
                break;
            }
            INFRA_LOG_INFO("[edgex] [get_position] [success]");
            UMSymbolPosition positions;
            parse_position(doc, positions);
            return positions;
        } catch (const std::exception& ex) {
            INFRA_LOG_WARN("[edgex] [get_position] [exception], msg: {}", ex.what());
        }
    } while (0);
    INFRA_LOG_WARN("[edgex] [get_position] [fail], recv: {}", response);
    return {};
}

void EdgexAccount::get_balance(const Currency& currency, BalanceCallback cb) {
    if (balance_path_.empty()) {
        cb(Errno::NotImplemented, {});
        return;
    }
    std::string query{};
    query.append("accountId=").append(g_account_id);
    auto req = get_request_body_with_sign(HTTP_GET, rest_host_, balance_path_, query, account_secret_);
    rest_.send(req, [this, currency, cb](HttpResponseBody& res) {
        std::string response = boost::beast::buffers_to_string(res.body().data());

        do {
            if (res.result() != HTTP_STATUS_OK) {
                break;
            }
            try {
                PARSE_JSON(response, doc);
                if (doc["code"].error() != simdjson::SUCCESS) {
                    break;
                }
                std::string_view code_str = doc["code"];
                if (code_str != SUCCESS_CODE) {
                    break;
                }
                INFRA_LOG_INFO("[edgex] [get_balance] [success]");
                UMCurrencyBalance assets;
                parse_balance(doc, currency, assets);
                cb(Errno::Ok, assets);
                return;
            } catch (const std::exception& ex) {
                INFRA_LOG_WARN("[edgex] [get_balance] [exception], msg: {}", ex.what());
            }
        } while (0);
        INFRA_LOG_WARN("[edgex] [get_balance] [fail], recv: {}", response);
        cb(extract_error_code(response), {});
    });
}

void EdgexAccount::get_position(const Symbol& symbol, PositionCallback cb) {
    if (position_path_.empty()) {
        cb(Errno::NotImplemented, {});
        return;
    }

    if (symbol.empty()) {
        INFRA_LOG_WARN("[edgex] [get_position] [fail], msg: symbol is empty");
        cb(Errno::InvalidParams, {});
        return;
    }

    std::string query{};
    query.append("accountId=").append(g_account_id);
    std::string contractId;
    if (!get_contract_id(symbol, contractId)) {
        INFRA_LOG_WARN("[edgex] [get_position] [fail], msg: get contract id failed for pair {}", symbol);
        cb(Errno::InvalidParams, {});
        return;
    }
    query.append("&contractIdList=").append(contractId);
    auto req = get_request_body_with_sign(HTTP_GET, rest_host_, position_path_, query, account_secret_);
    rest_.send(req, [this, cb](HttpResponseBody& res) {
        std::string response = boost::beast::buffers_to_string(res.body().data());
        do {
            if (res.result() != HTTP_STATUS_OK) {
                break;
            }
            try {
                PARSE_JSON(response, doc);
                if (doc["code"].error() != simdjson::SUCCESS) {
                    break;
                }
                std::string_view code_str = doc["code"];
                if (code_str != SUCCESS_CODE) {
                    break;
                }
                UMSymbolPosition positions;
                parse_position(doc, positions);
                INFRA_LOG_INFO("[edgex] [get_position] [success] num: {}", positions.size());
                cb(Errno::Ok, positions);
                return;
            } catch (const std::exception& ex) {
                INFRA_LOG_WARN("[edgex] [get_position] [exception], msg: {}", ex.what());
            }
        } while (0);
        INFRA_LOG_WARN("[] [get_position] [fail], recv: {}", response);
        cb(extract_error_code(response), {});
    });
}

bool EdgexAccount::set_leverage(const Symbol& symbol, unsigned int leverage, MarginMode mode) {
    INFRA_LOG_WARN("[edgex] [set_leverage] [fail], not supported");
    return false;
}

bool EdgexAccount::set_margin_mode(const Symbol& symbol, MarginMode mode) {
    INFRA_LOG_WARN("[edgex] [set_margin_mode] [fail], not supported");
    return false;
}

bool EdgexAccount::set_position_mode(PositionMode mode) {
    INFRA_LOG_WARN("[edgex] [set_position_mode] [fail], not supported");
    return false;
}
} // namespace infra
