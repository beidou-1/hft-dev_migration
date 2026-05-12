#include "gate_utils.h"
#include "exchanges/signature.h"

namespace infra::gate {
Errno extract_error_code(std::string_view sv) {
    if (sv.find("timeout") != std::string_view::npos) {
        return Errno::RequestTimeout;
    } else if (sv.find("API-key") != std::string_view::npos) {
        return Errno::AuthFailed;
    } else if (sv.find("insufficient") != std::string_view::npos ||
               sv.find("INSUFFICIENT_AVAILABLE") != std::string_view::npos) {
        return Errno::InsufficientBalance;
    } else if (sv.find("ORDER_NOT_FOUND") != std::string_view::npos) {
        return Errno::OrderNotFound;
    } else if (sv.find("is too small") != std::string_view::npos) {
        return Errno::SmallSizeOrder;
    } else {
        return Errno::UnknownError;
    }
}

Currency get_right_currency(const Currency& currency) { return to_lower_str(currency); }

HttpRequestBody get_request_body_with_sign(boost::beast::http::verb method, const std::string& host,
                                           const std::string& path, const std::string& query, const std::string& body,
                                           const AccountSecret& secret) {
    using namespace boost::beast;
    std::string timestamp = std::to_string(time_get_now_sec());
    std::string url_str = query.empty() ? path : (path + "?" + query);

    std::string hashed_payload = generate_hash_sha512(body);
    std::string pre_sign =
        std::string(http::to_string(method)) + "\n" + path + "\n" + query + "\n" + hashed_payload + "\n" + timestamp;
    std::string signature = generate_sign_hmac512(secret.api_secret, pre_sign);

    HttpRequestBody req{method, url_str, 11};
    req.set(http::field::host, host);
    req.set(http::field::user_agent, BOOST_BEAST_VERSION_STRING);

    req.set("KEY", secret.api_key);
    req.set("Timestamp", timestamp);
    req.set("SIGN", signature);
    req.set("X-Gate-Size-Decimal", "1");

    if (method == http::verb::post || method == http::verb::put || method == http::verb::patch) {
        req.set(http::field::content_type, "application/json");
        req.body() = body;
        req.prepare_payload();
    }
    return req;
}

std::string get_websocket_sign(const std::string& channel, const std::string& event, const std::string& timestamp,
                               const AccountSecret& secret) {
    std::string pre_sign = "channel=" + channel + "&event=" + event + "&time=" + timestamp;
    return generate_sign_hmac512(secret.api_secret, pre_sign);
}

std::string get_ws_api_sign(const std::string& channel, const std::string& request_param, const std::string& timestamp,
                            const AccountSecret& secret) {
    std::string pre_sign = "api\n" + channel + "\n" + request_param + "\n" + timestamp;
    return generate_sign_hmac512(secret.api_secret, pre_sign);
}

void parse_balance(const simdjson::dom::element& doc, const Currency& currency, UMCurrencyBalance& res) {
    res.clear();
    std::string_view currency_text = doc["currency"];
    std::string asset = to_lower_str(std::string(currency_text));
    if (!currency.empty() && !compare_currency(asset, currency))
        return;

    std::string_view order_margin_text = doc["order_margin"];
    std::string_view cross_available_text = doc["cross_available"];
    std::string_view position_initial_margin_text = doc["position_initial_margin"];

    double available = str_to_float(cross_available_text);
    double frozen = str_to_float(order_margin_text) + str_to_float(position_initial_margin_text);

    auto account_asset = std::make_shared<Balance>(asset, available, frozen);
    account_asset->withdraw = available;
    res[account_asset->currency] = account_asset;
}

void parse_position(const simdjson::dom::element& doc, UMSymbolPosition& res) {
    res.clear();
    simdjson::dom::array array = doc.get_array();
    for (auto&& item : array) {
        std::string_view value_text = item["value"];
        if (value_text == "0")
            continue;

        std::string_view symbol_text = item["contract"];
        std::string_view entry_price_text = item["entry_price"];
        std::string_view size_text = item["size"];
        std::string_view mode_text = item["mode"];
        std::string_view cross_leverage_text = item["cross_leverage_limit"];
        std::string_view liq_price_text = item["liq_price"];

        std::string pair = transfer_to_infra_pair(symbol_text);
        double entry_price = str_to_float(entry_price_text);
        double size = str_to_float(size_text);
        double position = size * g_pairs_info_cache[pair]->denomination_value;

        SpPosition pos_info{nullptr};
        auto it = res.find(pair);
        if (it == res.end()) {
            pos_info = std::make_shared<Position>();
            pos_info->position_mode = (mode_text == "single") ? PositionMode::one_way_mode : PositionMode::hedge_mode;
            pos_info->symbol = pair;
            pos_info->bankrupt_price = str_to_float(liq_price_text);
            pos_info->leverage = std::stoi(std::string(cross_leverage_text));
            pos_info->update_time = time_get_now_milli();
            res[pos_info->symbol] = pos_info;
        } else {
            pos_info = it->second;
            pos_info->update_time = time_get_now_milli();
        }

        if (mode_text == "dual_long") {
            pos_info->long_size = position;
            pos_info->long_open_price = entry_price;
        } else if (mode_text == "dual_short") {
            pos_info->short_size = -position;
            pos_info->short_open_price = entry_price;
        } else if (mode_text == "single") {
            if (position > 0) {
                pos_info->long_size = position;
                pos_info->long_open_price = entry_price;
            } else if (position < 0) {
                pos_info->short_size = -position;
                pos_info->short_open_price = entry_price;
            }
        }
    }
}

SpOrder parse_rtn_order(const simdjson::dom::object& obj, std::string_view channel) {
    std::string_view symbol_text = obj["contract"];
    std::string_view order_status_text = obj["status"];
    std::string_view text_text = "";
    auto text_res = obj["text"].get<std::string_view>();
    if (text_res.error() == simdjson::SUCCESS)
        text_text = text_res.value();

    Symbol pair = transfer_to_infra_pair(symbol_text);
    ClientOrderId client_oid(text_text);
    client_oid.erase(0, 2); // 去掉 "t-" 前缀

    OrderId market_oid(std::to_string(obj["id"].get_int64().value()));
    auto rtn_order = std::make_shared<Order>(pair, client_oid, market_oid);

    std::string_view left_text = obj["left"];
    std::string_view size_text = obj["size"];
    double left = double_abs(str_to_float(left_text) * g_pairs_info_cache[pair]->denomination_value);
    rtn_order->quantity = double_abs(str_to_float(size_text) * g_pairs_info_cache[pair]->denomination_value);

    OrderStatus order_status = OrderStatus::New;
    if (order_status_text == "open" && left == rtn_order->quantity)
        order_status = OrderStatus::New;
    else if (order_status_text == "open" && left != rtn_order->quantity)
        order_status = OrderStatus::PartiallyFilled;
    else if (order_status_text == "finished" && left != double(0))
        order_status = OrderStatus::Canceled;
    else if (order_status_text == "finished" && left == double(0))
        order_status = OrderStatus::Filled;

    rtn_order->status = order_status;
    if (channel == "futures.orders") {
        rtn_order->price = double(obj["price"].get_double().value());
        rtn_order->avg_price = double(obj["fill_price"].get_double().value());
        rtn_order->exchange_create_time = obj["create_time_ms"];
    } else if (channel == "query_order") {
        std::string_view price_text = obj["price"];
        std::string_view fill_price_text = obj["fill_price"];
        rtn_order->price = str_to_float(price_text);
        rtn_order->avg_price = str_to_float(fill_price_text);
        rtn_order->exchange_create_time = static_cast<int64_t>(obj["create_time"].get_double() * 1000.0);
    }

    rtn_order->cum_deal_base = rtn_order->quantity - left;
    rtn_order->cum_deal_quote = rtn_order->cum_deal_base * rtn_order->avg_price;

    int64_t update_time = 0;
    auto update_time_res = obj["update_time"].get_int64();
    if (update_time_res.error() == simdjson::SUCCESS)
        update_time = update_time_res.value();
    rtn_order->exchange_update_time = update_time;
    return rtn_order;
}

SpFundingFee parse_funding_fee(const simdjson::dom::element& doc, const Symbol& pair) {
    simdjson::dom::array array = doc.get_array();
    for (auto&& item : array) {
        std::string_view funding_rate_text = item["r"];
        double fee = str_to_float(funding_rate_text);
        return std::make_shared<FundingFee>(pair, time_get_now_milli(), fee);
    }
    return nullptr;
}

void parse_pairs_info(const simdjson::dom::element& doc, const Currency& currency) {
    g_pairs_info_cache.clear();
    g_all_symbols.clear();
    simdjson::dom::array array = doc.get_array();
    for (auto&& item : array) {
        std::string_view symbol_text = item["name"];
        std::string_view status_text;
        if (item["status"].get(status_text) == simdjson::SUCCESS && status_text != "trading")
            continue;

        std::string pair = transfer_to_infra_pair(symbol_text);
        std::string_view order_size_min_text = item["order_size_min"];
        std::string_view order_price_round_text = item["order_price_round"];
        std::string_view quanto_multiplier_text = item["quanto_multiplier"];
        double min_size = str_to_float(order_size_min_text);
        double multiplier = str_to_float(quanto_multiplier_text);

        auto pair_info = std::make_shared<ExchangePairInfo>();
        pair_info->pair = pair;
        pair_info->trading_min_base = min_size * multiplier;
        pair_info->step_size_base = pair_info->trading_min_base;
        pair_info->step_size_quote = str_to_float(order_price_round_text);
        pair_info->denomination_value = multiplier;
        pair_info->alias = symbol_text;

        g_pairs_info_cache[pair] = pair_info;
        g_all_symbols.push_back(pair);
    }
}

double parse_margin_ratio(const simdjson::dom::element& doc) {
    std::string_view maintenance_margin_sv = doc["cross_maintenance_margin"];
    std::string_view available_sv = doc["cross_available"];
    std::string_view unrealised_pnl_sv = doc["cross_unrealised_pnl"];

    double maintenance_margin = str_to_float(maintenance_margin_sv);
    double available = str_to_float(available_sv);
    double unrealised_pnl = str_to_float(unrealised_pnl_sv);
    if (is_zero(maintenance_margin)) {
        return 999.0;
    } else if (is_zero(available + unrealised_pnl)) {
        return 0.0;
    }
    double mgnRatio = (available + unrealised_pnl) / maintenance_margin;
    return mgnRatio;
}

} // namespace infra::gate
