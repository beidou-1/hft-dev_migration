#include "weex_account.h"
using namespace infra::weex;

namespace infra {
bool WeexAccount::initialize() {
    auto& info = g_config_map[base_config_.to_str()];
    if (info.empty()) {
        INFRA_LOG_WARN("[weex] [initialize] [fail], msg: {} {} {} not implemented",
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

void WeexAccount::get_balance(const Currency& currency, BalanceCallback cb) {
    if (balance_path_.empty()) {
        cb(Errno::NotImplemented, {});
        return;
    }

    auto req = get_request_body_with_sign(HTTP_GET, rest_host_, balance_path_, "", "", account_secret_);
    rest_.send(req, [this, currency, cb](HttpResponseBody& res) {
        std::string response = boost::beast::buffers_to_string(res.body().data());
        do {
            if (res.result() != HTTP_STATUS_OK)
                break;
            try {
                PARSE_JSON(response, doc);
                if (doc["code"].error() == simdjson::SUCCESS && doc["code"].get_string() != SUCCESS_CODE)
                    break;
                INFRA_LOG_INFO("[weex] [get_balance] [success], recv: {}", response);
                UMCurrencyBalance assets;
                parse_balance(doc, currency, assets);
                cb(Errno::Ok, assets);
                return;
            } catch (const std::exception& ex) {
                INFRA_LOG_WARN("[weex] [get_balance] [exception], msg: {}", ex.what());
            }
        } while (0);
        INFRA_LOG_WARN("[weex] [get_balance] [fail], recv: {}", response);
        cb(extract_error_code(response), {});
    });
}

void WeexAccount::get_position(const Symbol& symbol, PositionCallback cb) {
    if (position_path_.empty()) {
        cb(Errno::NotImplemented, {});
        return;
    }

    std::string path = position_path_ + (symbol.empty() ? "/allPosition" : "/singlePosition");
    std::string query = symbol.empty() ? "" : "symbol=" + transfer_from_infra_pair(symbol);
    auto req = get_request_body_with_sign(HTTP_GET, rest_host_, path, query, "", account_secret_);
    rest_.send(req, [this, cb](HttpResponseBody& res) {
        std::string response = boost::beast::buffers_to_string(res.body().data());
        do {
            if (res.result() != HTTP_STATUS_OK)
                break;
            try {
                PARSE_JSON(response, doc);
                if (doc["code"].error() == simdjson::SUCCESS && doc["code"].get_string() != SUCCESS_CODE)
                    break;
                INFRA_LOG_INFO("[weex] [get_position] [success], recv: {}", response);
                UMSymbolPosition positions;
                parse_position(doc, positions);
                cb(Errno::Ok, positions);
                return;
            } catch (const std::exception& ex) {
                INFRA_LOG_WARN("[weex] [get_position] [exception], msg: {}", ex.what());
            }
        } while (0);
        INFRA_LOG_WARN("[weex] [get_position] [fail], recv: {}", response);
        cb(extract_error_code(response), {});
    });
}

void WeexAccount::set_leverage(const Symbol& symbol, unsigned int leverage, MarginMode mode, LeverageCallback cb) {
    if (leverage_path_.empty() || symbol.empty() || leverage == 0)
        return;

    std::string payload{};
    if (mode == MarginMode::CROSS) {
        payload = fmt::format(R"({{"symbol":"{}","marginType":"CROSSED","crossLeverage":"{}"}})",
                              transfer_from_infra_pair(symbol), leverage);
    } else {
        payload = fmt::format(
            R"({{"symbol":"{}","marginType":"ISOLATED","isolatedLongLeverage":"{}","isolatedShortLeverage":"{}"}})",
            transfer_from_infra_pair(symbol), leverage, leverage);
    }

    auto req = get_request_body_with_sign(HTTP_POST, rest_host_, leverage_path_, "", payload, account_secret_);
    rest_.send(req, [this, cb, leverage, symbol](HttpResponseBody& res) {
        std::string msg = boost::beast::buffers_to_string(res.body().data());
        do {
            if (res.result() != HTTP_STATUS_OK)
                break;
            try {
                PARSE_JSON(msg, doc);
                if (doc["code"].error() == simdjson::SUCCESS && doc["code"].get_string().value() == SUCCESS_CODE) {
                    INFRA_LOG_INFO("[okex] [set_leverage] [success], msg: set leverage {} for symbol {}", leverage,
                                   symbol);
                    cb(Errno::Ok);
                    return;
                }
            } catch (const std::exception& ex) {
                INFRA_LOG_WARN("[okex] [set_leverage] [exception], exception: {}", ex.what());
            }
        } while (0);
        INFRA_LOG_WARN("[okex] [set_leverage] [fail], response: {}", msg);
        cb(extract_error_code(msg));
    });
}
void WeexAccount::get_margin_ratio(MarginRatioCallback cb) {
    if (balance_path_.empty()) {
        cb(Errno::NotImplemented, 0);
        return;
    }

    auto req = get_request_body_with_sign(HTTP_GET, rest_host_, balance_path_, "", "", account_secret_);
    rest_.send(req, [this, cb](HttpResponseBody& res) {
        std::string msg = boost::beast::buffers_to_string(res.body().data());
        do {
            if (res.result() != HTTP_STATUS_OK)
                break;
            try {
                PARSE_JSON(msg, doc);
                if (doc["code"].error() == simdjson::SUCCESS && doc["code"].get_string() != SUCCESS_CODE)
                    break;
                INFRA_LOG_INFO("[weex] [get_margin_ratio] [success]");
                cb(Errno::Ok, parse_margin_ratio(doc));
                return;
            } catch (const std::exception& ex) {
                INFRA_LOG_WARN("[weex] [get_margin_ratio] [exception], exception: {}", ex.what());
            }
        } while (0);
        INFRA_LOG_WARN("[weex] [get_margin_ratio] [fail], response: {}", msg);
        cb(extract_error_code(msg), 0);
    });
}

} // namespace infra
