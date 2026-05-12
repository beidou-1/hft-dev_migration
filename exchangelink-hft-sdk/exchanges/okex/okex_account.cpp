#include "okx_account.h"
#include "okx_utils.h"
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
    margin_mode_path_ = info[MARGIN_MODE_PATH];
    position_mode_path_ = info[POSITION_MODE_PATH];
    return true;
}

UMCurrencyBalance OkxAccount::get_balance(const Currency& currency) {
    if (balance_path_.empty()) {
        return {};
    }

    auto req = get_request_body_with_sign(http::verb::get, rest_host_, balance_path_, "", account_secret_);
    boost::beast::error_code ec;
    std::string response = rest_.sync_send(req, ec);
    if (ec) {
        INFRA_LOG_WARN("[okex] [get_balance] [fail], response: {}", response);
        return {};
    }
    UMCurrencyBalance assets;
    parse_balance(currency, response, assets);
    return assets;
}

UMSymbolPosition OkxAccount::get_position(const Symbol& symbol) {
    if (position_path_.empty()) {
        return {};
    }

    std::string query{};
    query.append("instType=SWAP");
    if (!symbol.empty()) {
        query.append("&instId=").append(transfer_from_infra_pair(symbol));
    }
    auto req = get_request_body_with_sign(http::verb::get, rest_host_, position_path_, query, account_secret_);

    boost::beast::error_code ec;
    std::string response = rest_.sync_send(req, ec);
    if (ec || (strstr(response.c_str(), "timeout") != nullptr) || (strstr(response.c_str(), "API-key") != nullptr)) {
        INFRA_LOG_WARN("[okex] [get_position] [fail], response: {}", response);
        return {};
    }
    UMSymbolPosition positions;
    parse_position(response, positions);
    return positions;
}

void OkxAccount::get_balance(const Currency& currency, BalanceCallback cb) {
    if (balance_path_.empty()) {
        return;
    }

    auto req = get_request_body_with_sign(http::verb::get, rest_host_, balance_path_, "", account_secret_);
    rest_.send(req, [this, currency, cb](HttpResponseBody& res) {
        std::string response = boost::beast::buffers_to_string(res.body().data());
        if (LIKELY(res.result() == http::status::ok)) {
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
        if (LIKELY(res.result() == http::status::ok)) {
            UMSymbolPosition positions;
            parse_position(response, positions);
            cb(Errno::Ok, positions);
        } else {
            INFRA_LOG_WARN("[okex] [get_position] [fail], response: {}", response);
            cb(extract_error_msg(response), {});
        }
    });
}

bool OkxAccount::set_leverage(const Symbol& symbol, unsigned int leverage, MarginMode mode) {
    std::string query = fmt::format(R"({{"lever":"{}","mgnMode":"cross","instId":"{}", "ccy":"USDT"}})", leverage,
                                    transfer_from_infra_pair(symbol));

    auto req = get_request_body_with_sign(http::verb::post, rest_host_, leverage_path_, query, account_secret_);
    if (!send_http_request_sync(req, "set_leverage")) {
        return false;
    }
    INFRA_LOG_INFO("[okex] [set_leverage] [success], msg: set leverage {} for symbol {}", leverage, symbol);
    return true;
}

bool OkxAccount::set_margin_mode(const Symbol& symbol, MarginMode mode) {
    INFRA_LOG_WARN("[okex] [set_margin_mode] [fail], not supported");
    return false;
}

bool OkxAccount::set_position_mode(PositionMode mode) {
    if (position_mode_path_.empty()) {
        return false;
    }

    std::string query =
        fmt::format(R"({{"posMode":"{}"}})", (mode == PositionMode::one_way_mode ? "net_mode" : "long_short_mode"));
    auto req = get_request_body_with_sign(boost::beast::http::verb::post, rest_host_, position_mode_path_, query,
                                          account_secret_);
    if (!send_http_request_sync(req, "set_position_mode")) {
        return false;
    }
    INFRA_LOG_INFO("[okex] [set_position_mode] [success], msg: position mode set to {}", to_string(mode));
    g_current_position_mode = mode;
    return true;
}

bool OkxAccount::send_http_request_sync(HttpRequestBody& req, const std::string& function_name) {
    boost::beast::error_code ec;
    std::string response = rest_.sync_send(req, ec);
    if (ec) {
        INFRA_LOG_WARN("[okex] [{}] [fail], response: {}", function_name, response);
        return false;
    }
    try {
        PARSE_JSON(response, doc);
        if (doc["code"].error() == simdjson::SUCCESS) {
            std::string_view code = doc["code"];
            if (code != "0") {
                INFRA_LOG_WARN("[okex] [{}] [fail], response: {}", function_name, response);
                return false;
            }
        }
    } catch (const std::exception& ex) {
        INFRA_LOG_WARN("[okex] [{}] [exception], parse error: {}, response: {}", function_name, ex.what(), response);
        return false;
    }
    return true;
}
} // namespace infra
