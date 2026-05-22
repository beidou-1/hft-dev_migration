#include "bitunix_account.h"
using namespace infra::bitunix;

namespace infra {
bool BitunixAccount::initialize() {
    auto& info = g_config_map[base_config_.to_str()];
    if (info.empty()) {
        INFRA_LOG_WARN("[bitunix] [initialize] [fail], msg: {} {} {} not implemented",
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

void BitunixAccount::get_balance(const Currency& currency, BalanceCallback cb) {
    if (balance_path_.empty()) {
        cb(Errno::NotImplemented, {});
        return;
    }

    auto req = get_request_body_with_sign(HTTP_GET, rest_host_, balance_path_, "marginCoin=USDT", "", account_secret_);
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
                INFRA_LOG_INFO("[bitunix] [get_balance] [success], recv: {}", response);
                UMCurrencyBalance assets;
                parse_balance(doc, currency, assets);
                cb(Errno::Ok, assets);
                return;
            } catch (const std::exception& ex) {
                INFRA_LOG_WARN("[bitunix] [get_balance] [exception], msg: {}", ex.what());
            }
        } while (0);
        INFRA_LOG_WARN("[bitunix] [get_balance] [fail], recv: {}", response);
        cb(extract_error_code(response), {});
    });
}

void BitunixAccount::get_position(const Symbol& symbol, PositionCallback cb) {
    if (position_path_.empty()) {
        cb(Errno::NotImplemented, {});
        return;
    }

    std::string query = symbol.empty() ? "" : "symbol=" + transfer_from_infra_pair(symbol);
    auto req = get_request_body_with_sign(HTTP_GET, rest_host_, position_path_, query, "", account_secret_);
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
                INFRA_LOG_INFO("[bitunix] [get_position] [success], recv: {}", response);
                UMSymbolPosition positions;
                parse_position(doc, positions);
                cb(Errno::Ok, positions);
                return;
            } catch (const std::exception& ex) {
                INFRA_LOG_WARN("[bitunix] [get_position] [exception], msg: {}", ex.what());
            }
        } while (0);
        INFRA_LOG_WARN("[bitunix] [get_position] [fail], recv: {}", response);
        cb(extract_error_code(response), {});
    });
}

void BitunixAccount::set_leverage(const Symbol& symbol, unsigned int leverage, MarginMode mode, LeverageCallback cb) {

    std::string payload = fmt::format(R"({{"symbol":"{}","leverage":{},"marginCoin":"USDT"}})",
                                      transfer_from_infra_pair(symbol), leverage);
    auto req = get_request_body_with_sign(HTTP_POST, rest_host_, leverage_path_, "", payload, account_secret_);
    rest_.send(req, [this, cb, leverage, symbol](HttpResponseBody& res) {
        std::string msg = boost::beast::buffers_to_string(res.body().data());
        do {
            if (res.result() != HTTP_STATUS_OK)
                break;
            try {
                PARSE_JSON(msg, doc);
                if (doc["code"].get_int64() == SUCCESS_CODE) {
                    INFRA_LOG_INFO("[bitunix] [set_leverage] [success], msg: set leverage {} for symbol {}", leverage,
                                   symbol);
                    cb(Errno::Ok);
                    return;
                }
            } catch (const std::exception& ex) {
                INFRA_LOG_WARN("[bitunix] [set_leverage] [exception], exception: {}", ex.what());
            }
        } while (0);
        INFRA_LOG_WARN("[bitunix] [set_leverage] [fail], response: {}", msg);
        cb(extract_error_code(msg));
    });
}

void BitunixAccount::get_margin_ratio(MarginRatioCallback cb) {
    if (balance_path_.empty()) {
        cb(Errno::NotImplemented, 0);
        return;
    }

    auto req = get_request_body_with_sign(HTTP_GET, rest_host_, balance_path_, "marginCoin=USDT", "", account_secret_);
    rest_.send(req, [this, cb](HttpResponseBody& res) {
        std::string msg = boost::beast::buffers_to_string(res.body().data());
        do {
            if (res.result() != HTTP_STATUS_OK)
                break;
            try {
                PARSE_JSON(msg, doc);
                if (doc["code"].get_int64() != SUCCESS_CODE)
                    break;
                INFRA_LOG_INFO("[bitunix] [get_margin_ratio] [success]");
                cb(Errno::Ok, parse_margin_ratio(doc));
                return;
            } catch (const std::exception& ex) {
                INFRA_LOG_WARN("[bitunix] [get_margin_ratio] [exception], msg: {}", ex.what());
            }
        } while (0);
        INFRA_LOG_WARN("[bitunix] [get_margin_ratio] [fail], recv: {}", msg);
        cb(extract_error_code(msg), 0);
    });
}

} // namespace infra
