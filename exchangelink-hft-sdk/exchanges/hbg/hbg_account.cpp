#include "hbg_account.h"
using namespace infra::hbg;

namespace infra {
bool HbgAccount::initialize() {
    auto& info = g_config_map[base_config_.to_str()];
    if (info.empty()) {
        INFRA_LOG_WARN("[hbg] [initialize] [fail], msg: {} {} {} not implemented", to_string(base_config_.account_type),
                       to_string(base_config_.address_type), to_string(base_config_.settle_unit));
        return false;
    }

    rest_host_ = info[REST_HOST];
    balance_path_ = info[BALANCE_PATH];
    position_path_ = info[POSITION_PATH];
    leverage_path_ = info[LEVERAGE_PATH];
    return true;
}

void HbgAccount::get_margin_ratio(MarginRatioCallback cb) {
    auto req = get_request_body_with_sign(HTTP_GET, rest_host_, balance_path_, "", "", account_secret_);
    client_.send(req, [this, cb](HttpResponseBody& res) {
        std::string msg = boost::beast::buffers_to_string(res.body().data());
        handle_rest_response(
            res, msg, "get_margin_ratio",
            [&](auto& doc) {
                if (doc["code"].get_int64().value() != 200 || doc["message"].get_string().value() != "Success") {
                    return false;
                }
                INFRA_LOG_INFO("[hbg] [get_margin_ratio] [success], recv: {}", msg);
                cb(Errno::Ok, parse_margin_ratio(doc));
                return true;
            },
            [&]() { cb(extract_error_code(msg), double(0)); });
    });
}

void HbgAccount::get_balance(const Currency& currency, BalanceCallback cb) {
    std::string settle = currency.empty() ? "usdt" : get_right_currency(currency);
    auto req = get_request_body_with_sign(HTTP_GET, rest_host_, balance_path_, "", "", account_secret_);
    client_.send(req, [this, settle, cb](HttpResponseBody& res) {
        std::string msg = boost::beast::buffers_to_string(res.body().data());
        handle_rest_response(
            res, msg, "get_balance",
            [&](auto& doc) {
                if (doc["code"].get_int64().value() != 200 || doc["message"].get_string().value() != "Success") {
                    return false;
                }
                INFRA_LOG_INFO("[hbg] [get_balance] [success], recv: {}", msg);
                UMCurrencyBalance assets;
                parse_balance(doc, settle, assets);
                cb(Errno::Ok, assets);
                return true;
            },
            [&]() { cb(extract_error_code(msg), {}); });
    });
}

void HbgAccount::get_position(const Symbol& symbol, PositionCallback cb) {
    auto req = get_request_body_with_sign(HTTP_GET, rest_host_, position_path_, "", "", account_secret_);
    client_.send(req, [this, cb](HttpResponseBody& res) {
        std::string msg = boost::beast::buffers_to_string(res.body().data());
        handle_rest_response(
            res, msg, "get_position",
            [&](auto& doc) {
                if (doc["code"].get_int64().value() != 200 || doc["message"].get_string().value() != "Success") {
                    return false;
                }
                INFRA_LOG_INFO("[hbg] [get_position] [success], recv: {}", msg);
                UMSymbolPosition positions;
                parse_position(doc, positions);
                cb(Errno::Ok, positions);
                return true;
            },
            [&]() { cb(extract_error_code(msg), {}); });
    });
}

void HbgAccount::set_leverage(const Symbol& symbol, unsigned int leverage, MarginMode mode, LeverageCallback cb) {
    if (symbol.empty() || leverage < 1) {
        cb(Errno::InvalidParams);
        return;
    }

    std::string margin_mode;
    if (mode == MarginMode::CROSS) {
        margin_mode = "cross";
    } else {
        INFRA_LOG_WARN("[hbg] [set_leverage] [fail], margin_mode must be cross.");
        return;
    }

    std::string symbol_ex = transfer_from_infra_pair(symbol);
    
    std::string query =
        fmt::format("contract_code={}&margin_mode={}&lever_rate={}", symbol_ex, margin_mode, leverage);
    auto req = get_request_body_with_sign(HTTP_POST, rest_host_, leverage_path_, query, "", account_secret_);
    client_.send(req, [this, leverage, mode, cb](HttpResponseBody& res) {
        std::string msg = boost::beast::buffers_to_string(res.body().data());
        handle_rest_response(
            res, msg, "set_leverage",
            [&](auto& doc) {
                if (doc["code"].get_int64().value() != 200 || doc["message"].get_string().value() != "Success") {
                    return false;
                }
                unsigned int rt_cross_leverage = doc["data"]["lever_rate"].get_int64().value();
                if (mode == MarginMode::CROSS && rt_cross_leverage == leverage) {
                    INFRA_LOG_INFO("[hbg] [set_leverage] [success], recv: {}", msg);
                    cb(Errno::Ok);
                    return true;
                }
                return false;
            },
            [&]() { cb(extract_error_code(msg)); });
    });
    
}

} // namespace infra
