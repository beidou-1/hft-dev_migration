#include "bitget_utils.h"
#include "exchanges/signature.h"

namespace infra::bitget {
Errno extract_error_code(std::string_view sv) {
    if (sv.find("timeout") != std::string_view::npos) {
        return Errno::RequestTimeout;
    } else if (sv.find("sign signature error") != std::string_view::npos) {
        return Errno::AuthFailed;
    } else if (sv.find("insufficient") != std::string_view::npos || sv.find("Insufficient") != std::string_view::npos) {
        return Errno::InsufficientBalance;
    } else if (sv.find("invalid params") != std::string_view::npos) {
        return Errno::InvalidParams;
    } else if (sv.find("Invalid IP") != std::string_view::npos) {
        return Errno::NotInWhiteList;
    } else if (sv.find("Order does not exist") != std::string_view::npos) {
        return Errno::OrderNotFound;
    } else if (sv.find("ClientOrderId is duplicated") != std::string_view::npos) {
        return Errno::DuplicatedId;
    } else if (sv.find("The minimum order value") != std::string_view::npos) {
        return Errno::SmallSizeOrder;
    } else {
        return Errno::UnknownError;
    }
}

HttpRequestBody get_request_body_with_sign(boost::beast::http::verb method, const std::string& host,
                                           const std::string& path, const std::string& query,
                                           const AccountSecret& secret) {
    using namespace boost::beast;
    std::string url_str = (method == http::verb::get && !query.empty()) ? (path + "?" + query) : path;

    std::string timestamp = std::to_string(time_get_now_milli());
    std::string msg = timestamp + std::string(http::to_string(method)) + path +
                      (method == http::verb::get && !query.empty() ? ("?" + query) : query);
    std::string signature = generate_sign_hmac256_b64(secret.api_secret, msg);

    HttpRequestBody req{method, url_str, 11};
    req.set(http::field::host, host);
    req.set(http::field::user_agent, BOOST_BEAST_VERSION_STRING);

    req.set("ACCESS-KEY", secret.api_key);
    req.set("ACCESS-SIGN", signature);
    req.set("ACCESS-TIMESTAMP", timestamp);
    req.set("ACCESS-PASSPHRASE", secret.api_phrase);

    if (method == http::verb::get) {
        req.set(http::field::content_type, "application/x-www-form-urlencoded");
    } else if (method == http::verb::post) {
        req.set(http::field::content_type, "application/json");
        req.body() = query;
        req.prepare_payload();
    }
    return req;
}

void parse_funding_fee(const simdjson::dom::element& doc, SpFundingFee& res) {
    simdjson::dom::array array = doc["data"];
    for (auto&& item : array) {
        std::string_view fundingRate = item["fundingRate"];
        res->fee = str_to_float(fundingRate);
        break;
    }
}

void parse_pairs_info(const simdjson::dom::element& doc, const Currency& currency) {
    g_pairs_info_cache.clear();
    g_all_symbols.clear();
    simdjson::dom::array array = doc["data"];
    for (auto&& item : array) {
        std::string_view symbol_text = item["symbol"];
        std::string_view minOrderQty = item["minOrderQty"];
        std::string_view quantityMultiplier = item["quantityMultiplier"];
        std::string_view priceMultiplier = item["priceMultiplier"];
        std::string_view minOrderAmount = item["minOrderAmount"];

        Symbol pair = transfer_to_infra_pair(symbol_text);
        auto pair_info = std::make_shared<ExchangePairInfo>();
        pair_info->pair = pair;
        pair_info->trading_min_base = str_to_float(minOrderQty);
        pair_info->step_size_base = str_to_float(quantityMultiplier);
        pair_info->step_size_quote = str_to_float(priceMultiplier);
        pair_info->min_size_quote = str_to_float(minOrderAmount);
        pair_info->alias = symbol_text;

        g_pairs_info_cache[pair] = pair_info;
        g_all_symbols.push_back(std::move(pair));
    }
}

void parse_balance(const simdjson::dom::element& doc, const Currency& currency, UMCurrencyBalance& res) {
    res.clear();
    simdjson::dom::array array = doc["data"]["assets"];
    for (auto&& item : array) {
        std::string_view currency_text = item["coin"];
        std::string asset(currency_text);
        if (!currency.empty() && !compare_currency(asset, currency)) {
            continue;
        }

        std::string_view equity_text = item["equity"];
        std::string_view available_text = item["available"];
        std::string_view debt_text = item["debt"];
        double equity = str_to_float(equity_text);
        double available = str_to_float(available_text);
        double debt = str_to_float(debt_text);

        auto account_asset = std::make_shared<Balance>(asset, available, equity - available);
        account_asset->withdraw = available;
        account_asset->borrow = debt;
        res[account_asset->currency] = account_asset;
    }
}

void parse_position(const simdjson::dom::element& doc, UMSymbolPosition& res) {
    res.clear();
    simdjson::dom::object data = doc["data"];
    if (data["list"].is_null()) {
        return;
    }
    simdjson::dom::array array = data["list"];
    for (auto&& item : array) {
        std::string_view symbol_text = item["symbol"];
        std::string_view entryPrice_text = item["avgPrice"];
        std::string_view positionSide_text = item["posSide"];
        std::string_view holdMode_text = item["holdMode"];
        std::string_view positionAmt_text = item["total"];
        std::string_view marginType_text = item["marginMode"];
        std::string_view liquidationPrice_text = item["liquidationPrice"];
        std::string_view leverage_text = item["leverage"];

        std::string pair = transfer_to_infra_pair(symbol_text);
        double entry_price = str_to_float(entryPrice_text);
        double position_amount = str_to_float(positionAmt_text);

        SpPosition pos_info{nullptr};
        auto it = res.find(pair);
        if (it == res.end()) {
            pos_info = std::make_shared<Position>();
            pos_info->position_mode =
                (holdMode_text == "one_way_mode") ? PositionMode::one_way_mode : PositionMode::hedge_mode;
            pos_info->margin_mode = to_margin_mode(marginType_text);
            pos_info->symbol = pair;
            pos_info->bankrupt_price = str_to_float(liquidationPrice_text);
            pos_info->leverage = std::stoi(std::string(leverage_text));
            pos_info->update_time = time_get_now_milli();
            res[pos_info->symbol] = pos_info;
        } else {
            pos_info = it->second;
            pos_info->update_time = time_get_now_milli();
        }

        if (positionSide_text == "long") {
            pos_info->long_size = position_amount;
            pos_info->long_open_price = entry_price;
        } else if (positionSide_text == "short") {
            pos_info->short_size = position_amount;
            pos_info->short_open_price = entry_price;
        }
    }
}

double parse_margin_ratio(const simdjson::dom::element& doc) {
    std::string_view mgnRatio = doc["data"]["mgnRatio"];
    double ratio = std::stod(std::string(mgnRatio));
    return is_zero(ratio) ? 999.0 : 1 / ratio;
}

SpOrder parse_rtn_order(const simdjson::dom::object& obj) {
    std::string_view order_id = obj["orderId"];
    std::string_view symbol_text = obj["symbol"];
    std::string_view client_oid_text = obj["clientOid"];
    std::string_view order_status_text = obj["orderStatus"];
    std::string_view price_text = obj["price"];
    std::string_view quantity_text = obj["qty"];
    std::string_view accumulated_quantity_text = obj["cumExecQty"];
    std::string_view avg_price_text = obj["avgPrice"];
    std::string_view update_milli_text = obj["updatedTime"];
    int64_t update_milli = std::stoll(std::string(update_milli_text));
    std::string_view create_milli_text = obj["createdTime"];
    int64_t create_milli = std::stoll(std::string(create_milli_text));

    std::string pair = transfer_to_infra_pair(symbol_text);
    std::string client_oid(client_oid_text);
    std::string market_oid(order_id);
    SpOrder rtn_order = std::make_shared<Order>(pair, client_oid, market_oid);

    std::string status_text(order_status_text);
    std::transform(status_text.begin(), status_text.end(), status_text.begin(), ::toupper);
    OrderStatus order_status = to_order_status(status_text);
    if (order_status == OrderStatus::Expired) {
        order_status = OrderStatus::Canceled;
    }

    rtn_order->status = order_status;
    rtn_order->quantity = str_to_float(quantity_text);
    rtn_order->price = str_to_float(price_text);
    rtn_order->avg_price = str_to_float(avg_price_text);
    rtn_order->cum_deal_base = str_to_float(accumulated_quantity_text);
    rtn_order->cum_deal_quote = rtn_order->cum_deal_base * rtn_order->avg_price;
    rtn_order->exchange_create_time = create_milli;
    rtn_order->exchange_update_time = update_milli;
    return rtn_order;
}
} // namespace infra::bitget