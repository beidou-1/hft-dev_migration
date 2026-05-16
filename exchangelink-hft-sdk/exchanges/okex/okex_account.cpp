#include "okex_account.h"
#include "okex_utils.h"
using namespace infra::okex;
using namespace boost::beast;

namespace infra {
bool OkxAccount::initialize() {
    auto& info = g_config_map[base_config_.to_str()];
    if (info.empty()) {
        INFRA_LOG_WARN("[okex] [initialize] [fail], msg: {} {} {} not implemented", to_string(base_config_.account_type),
                       to_string(base_config_.address_type), to_string(base_config_.settle_unit));
        return false;
    }

    rest_host_ = info[REST_HOST];
    balance_path_ = info[BALANCE_PATH];
    position_path_ = info[POSITION_PATH];
    leverage_path_ = info[LEVERAGE_PATH];
    return true;
}

void OkxAccount::get_balance(const Currency& currency, BalanceCallback cb) {
    if (balance_path_.empty()) {
        return;
    }

    auto req = get_request_body_with_sign(http::verb::get, rest_host_, balance_path_, "", account_secret_);
    rest_.send(req, [this, currency, cb](HttpResponseBody& res) {
        std::string response = boost::beast::buffers_to_string(res.body().data());
        if (res.result() == http::status::ok) {
            UMCurrencyBalance assets;
            parse_balance(currency, response, assets);
            cb(Errno::Ok, assets);
        } else {
            INFRA_LOG_WARN("[okex] [get_balance] [fail], response: {}", response);
            cb(extract_error_msg(response), {});
        }
    });
}

void OkxAccount::get_position(const Symbol& symbol, PositionCallback cb) {
    if (position_path_.empty()) {
        return;
    }

    std::string query{};
    query.append("instType=SWAP");
    if (!symbol.empty()) {
        query.append("&instId=").append(transfer_from_infra_pair(symbol));
    }
    auto req = get_request_body_with_sign(http::verb::get, rest_host_, position_path_, query, account_secret_);
    rest_.send(req, [this, cb](HttpResponseBody& res) {
        std::string response = boost::beast::buffers_to_string(res.body().data());
        if (res.result() == http::status::ok) {
            UMSymbolPosition positions;
            parse_position(response, positions);
            cb(Errno::Ok, positions);
        } else {
            INFRA_LOG_WARN("[okex] [get_position] [fail], response: {}", response);
            cb(extract_error_msg(response), {});
        }
    });
}

void OkxAccount::set_leverage(const Symbol& symbol, unsigned int leverage, MarginMode mode, LeverageCallback cb) {
    std::string query = fmt::format(R"({{"lever":"{}","mgnMode":"cross","instId":"{}", "ccy":"USDT"}})", leverage,
                                    transfer_from_infra_pair(symbol));

    auto req = get_request_body_with_sign(http::verb::post, rest_host_, leverage_path_, query, account_secret_);
    rest_.send(req, [this, cb, leverage, symbol](HttpResponseBody& res) {
        std::string msg = boost::beast::buffers_to_string(res.body().data());
        do {
            if (res.result() != HTTP_STATUS_OK)
                break;
            try {
                PARSE_JSON(msg, doc);
                if (doc["code"].error() == simdjson::SUCCESS && doc["code"].get_string().value() == "0") {
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
        cb(extract_error_msg(msg));
    });
}

void OkxAccount::get_margin_ratio(MarginRatioCallback cb) {
    auto req = get_request_body_with_sign(http::verb::get, rest_host_, balance_path_, "", account_secret_);
    rest_.send(req, [this, cb](HttpResponseBody& res) {
        std::string msg = boost::beast::buffers_to_string(res.body().data());
        do {
            if (res.result() != http::status::ok)
                break;
            try {
                PARSE_JSON(msg, doc);
                if (doc["code"].error() == simdjson::SUCCESS && doc["code"].get_string().value() == "0") {
                    INFRA_LOG_INFO("[okex] [get_margin_ratio] [success]");
                    cb(Errno::Ok, parse_margin_ratio(doc));
                    return;
                }
            } catch (const std::exception& ex) {
                INFRA_LOG_WARN("[okex] [get_margin_ratio] [exception], exception: {}", ex.what());
            }
        } while (0);
        INFRA_LOG_WARN("[okex] [get_margin_ratio] [fail], response: {}", msg);
        cb(extract_error_msg(msg), 0);
    });
}

} // namespace infra
