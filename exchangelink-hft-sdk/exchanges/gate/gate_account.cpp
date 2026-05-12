#include "gate_account.h"
using namespace infra::gate;

namespace infra {
bool GateAccount::initialize() {
    auto& info = g_config_map[base_config_.to_str()];
    if (info.empty()) {
        INFRA_LOG_WARN("[gate] [initialize] [fail], msg: {} {} {} not implemented",
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

void GateAccount::get_margin_ratio(MarginRatioCallback cb) {
    std::string path = fmt::format("{}/usdt/accounts", balance_path_);
    auto req = get_request_body_with_sign(HTTP_GET, rest_host_, path, "", "", account_secret_);
    client_.send(req, [this, cb](HttpResponseBody& res) {
        std::string msg = boost::beast::buffers_to_string(res.body().data());
        handle_rest_response(
            res, msg, "get_margin_ratio",
            [&](auto& doc) {
                INFRA_LOG_INFO("[gate] [get_margin_ratio] [success], recv: {}", msg);
                cb(Errno::Ok, parse_margin_ratio(doc));
                return true;
            },
            [&]() { cb(extract_error_code(msg), double(0)); });
    });
}

void GateAccount::get_balance(const Currency& currency, BalanceCallback cb) {
    std::string settle = currency.empty() ? "usdt" : get_right_currency(currency);
    std::string path = fmt::format("{}/{}/accounts", balance_path_, settle);
    auto req = get_request_body_with_sign(HTTP_GET, rest_host_, path, "", "", account_secret_);
    client_.send(req, [this, settle, cb](HttpResponseBody& res) {
        std::string msg = boost::beast::buffers_to_string(res.body().data());
        handle_rest_response(
            res, msg, "get_balance",
            [&](auto& doc) {
                INFRA_LOG_INFO("[gate] [get_balance] [success], recv: {}", msg);
                UMCurrencyBalance assets;
                parse_balance(doc, settle, assets);
                cb(Errno::Ok, assets);
                return true;
            },
            [&]() { cb(extract_error_code(msg), {}); });
    });
}

void GateAccount::get_position(const Symbol& symbol, PositionCallback cb) {
    auto req = get_request_body_with_sign(HTTP_GET, rest_host_, position_path_, "", "", account_secret_);
    client_.send(req, [this, cb](HttpResponseBody& res) {
        std::string msg = boost::beast::buffers_to_string(res.body().data());
        handle_rest_response(
            res, msg, "get_position",
            [&](auto& doc) {
                if (!doc.is_array())
                    return false;
                INFRA_LOG_INFO("[gate] [get_position] [success], recv: {}", msg);
                UMSymbolPosition positions;
                parse_position(doc, positions);
                cb(Errno::Ok, positions);
                return true;
            },
            [&]() { cb(extract_error_code(msg), {}); });
    });
}

void GateAccount::set_leverage(const Symbol& symbol, unsigned int leverage, MarginMode mode, LeverageCallback cb) {
    if (symbol.empty() || leverage < 1) {
        cb(Errno::InvalidParams);
        return;
    }

    std::string path = fmt::format("{}/{}/leverage", leverage_path_, transfer_from_infra_pair(symbol));
    std::string query;
    if (mode == MarginMode::ISOLATED) {
        query = fmt::format("leverage={}", leverage);
    } else {
        query = fmt::format("cross_leverage_limit={}&leverage=0", leverage);
    }

    auto req = get_request_body_with_sign(HTTP_POST, rest_host_, path, query, "", account_secret_);
    client_.send(req, [this, leverage, mode, cb](HttpResponseBody& res) {
        std::string msg = boost::beast::buffers_to_string(res.body().data());
        handle_rest_response(
            res, msg, "set_leverage",
            [&](auto& doc) {
                unsigned int rt_leverage = std::stoul(std::string(doc["leverage"].get_string().value()));
                unsigned int rt_cross_leverage =
                    std::stoul(std::string(doc["cross_leverage_limit"].get_string().value()));
                if ((mode == MarginMode::ISOLATED && rt_leverage == leverage) ||
                    (mode == MarginMode::CROSS && rt_cross_leverage == leverage)) {
                    INFRA_LOG_INFO("[gate] [set_leverage] [success], recv: {}", msg);
                    cb(Errno::Ok);
                    return true;
                }
                return false;
            },
            [&]() { cb(extract_error_code(msg)); });
    });
}
} // namespace infra
