#include "bitget_account.h"
using namespace infra::bitget;

namespace infra {
bool BitgetAccount::initialize() {
    auto& info = g_config_map[base_config_.to_str()];
    if (info.empty()) {
        INFRA_LOG_WARN("[bitget] [initialize] [fail], msg: {} {} {} not implemented",
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

void BitgetAccount::get_margin_ratio(MarginRatioCallback cb) {
    auto req = get_request_body_with_sign(HTTP_GET, rest_host_, balance_path_, "", account_secret_);
    client_.send(req, [this, cb](HttpResponseBody& res) {
        std::string msg = boost::beast::buffers_to_string(res.body().data());
        handle_rest_response(
            res, msg, "get_margin_ratio",
            [&](auto& doc) {
                // INFRA_LOG_INFO("[bitget] [get_margin_ratio] [success], recv: {}", msg);
                cb(Errno::Ok, parse_margin_ratio(doc));
                return true;
            },
            [&]() { cb(extract_error_code(msg), 0); });
    });
}

void BitgetAccount::get_balance(const Currency& currency, BalanceCallback cb) {
    auto req = get_request_body_with_sign(HTTP_GET, rest_host_, balance_path_, "", account_secret_);
    client_.send(req, [this, currency, cb](HttpResponseBody& res) {
        std::string msg = boost::beast::buffers_to_string(res.body().data());
        handle_rest_response(
            res, msg, "get_balance",
            [&](auto& doc) {
                UMCurrencyBalance assets;
                parse_balance(doc, currency, assets);
                INFRA_LOG_INFO("[bitget] [get_balance] [success], size: {}", assets.size());
                cb(Errno::Ok, assets);
                return true;
            },
            [&]() { cb(extract_error_code(msg), {}); });
    });
}

void BitgetAccount::get_position(const Symbol& symbol, PositionCallback cb) {
    std::string query = "category=USDT-FUTURES";
    if (!symbol.empty()) {
        query.append("&symbol=").append(transfer_from_infra_pair(symbol));
    }
    auto req = get_request_body_with_sign(HTTP_GET, rest_host_, position_path_, query, account_secret_);
    client_.send(req, [this, cb](HttpResponseBody& res) {
        std::string msg = boost::beast::buffers_to_string(res.body().data());
        handle_rest_response(
            res, msg, "get_position",
            [&](auto& doc) {
                UMSymbolPosition positions;
                parse_position(doc, positions);
                INFRA_LOG_INFO("[bitget] [get_position] [success], size: {}", positions.size());
                cb(Errno::Ok, positions);
                return true;
            },
            [&]() { cb(extract_error_code(msg), {}); });
    });
}

void BitgetAccount::set_leverage(const Symbol& symbol, unsigned int leverage, MarginMode mode, LeverageCallback cb) {
    if (symbol.empty() || leverage < 1) {
        cb(Errno::InvalidParams);
        return;
    }

    std::string body = fmt::format(R"({{"category":"USDT-FUTURES","symbol":"{}","leverage":"{}"}})",
                                   transfer_from_infra_pair(symbol), leverage);
    auto req = get_request_body_with_sign(HTTP_POST, rest_host_, leverage_path_, body, account_secret_);
    client_.send(req, [this, cb](HttpResponseBody& res) {
        std::string msg = boost::beast::buffers_to_string(res.body().data());
        handle_rest_response(
            res, msg, "set_leverage",
            [&](auto& doc) {
                INFRA_LOG_INFO("[bitget] [set_leverage] [success], recv: {}", msg);
                cb(Errno::Ok);
                return true;
            },
            [&]() { cb(extract_error_code(msg)); });
    });
}
} // namespace infra