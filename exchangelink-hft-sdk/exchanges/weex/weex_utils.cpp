#include "weex_utils.h"

namespace infra::weex {

Errno extract_error_code(std::string_view sv) {
    if (sv.find("timeout") != std::string_view::npos) {
        return Errno::RequestTimeout;
    } else if (sv.find("signature") != std::string_view::npos) {
        return Errno::AuthFailed;
    } else if (sv.find("available amount not enoug") != std::string_view::npos) {
        return Errno::InsufficientBalance;
    } else if (sv.find("matches the stepSize") != std::string_view::npos) {
        return Errno::SmallSizeOrder;
    } else if (sv.find("Rate limit exceeded") != std::string_view::npos) {
        return Errno::RateLimitExceed;
    } else if (sv.find("Order does not exist") != std::string_view::npos) {
        return Errno::OrderNotFound;
    } else {
        return Errno::UnknownError;
    }
}

HttpRequestBody get_request_body_with_sign(boost::beast::http::verb method, const std::string& host,
                                           const std::string& path, const std::string& query, const std::string& body,
                                           const AccountSecret& secret) {
    using namespace boost::beast;
    std::string url_str = (method == http::verb::get && !query.empty()) ? (path + "?" + query) : path;

    std::string timestamp = std::to_string(time_get_now_milli());
    std::string msg = timestamp + std::string(http::to_string(method)) + path + (query.empty() ? body : ("?" + query));
    std::string signature = generate_sign_hmac256_b64(secret.api_secret, msg);

    HttpRequestBody req{method, url_str, 11};
    req.set(http::field::host, host);
    req.set(http::field::user_agent, BOOST_BEAST_VERSION_STRING);
    req.set(http::field::connection, "close");
    req.set(http::field::content_type, "application/json");

    if (method == http::verb::post) {
        req.body() = body;
        req.prepare_payload();
    }

    req.set("ACCESS-KEY", secret.api_key);
    req.set("ACCESS-SIGN", signature);
    req.set("ACCESS-PASSPHRASE", secret.api_phrase);
    req.set("ACCESS-TIMESTAMP", timestamp);
    return req;
}

void parse_balance(const simdjson::dom::element& doc, const Currency& currency, UMCurrencyBalance& res) {
    res.clear();
    simdjson::dom::array array = doc.get_array();
    for (auto item : array) {
        std::string_view coin_text = item["asset"];
        std::string asset(coin_text);
        // NOTE：currency值为空时返回所有，不为空时只返回currency对应的资产
        if (!currency.empty() && !compare_currency(asset, currency)) {
            continue;
        }

        std::string_view available_sv = item["availableBalance"];
        std::string_view frozen_sv = item["frozen"];

        double available = str_to_float(available_sv);
        double frozen = str_to_float(frozen_sv);
        auto account_asset = std::make_shared<Balance>(asset, available, frozen);
        account_asset->withdraw = available;
        res[account_asset->currency] = account_asset;
    }
}

void parse_position(const simdjson::dom::element& doc, UMSymbolPosition& res) {
    res.clear();
    simdjson::dom::array array = doc.get_array();
    for (auto item : array) {
        std::string_view symbol_text = item["symbol"];
        std::string_view side = item["side"];
        std::string_view size = item["size"];
        std::string_view open_value = item["openValue"];
        std::string_view liquidatePrice = item["liquidatePrice"];
        std::string_view leverage_text = item["leverage"];

        std::string pair = transfer_to_infra_pair(symbol_text);
        double entry_price = str_to_float(open_value) / str_to_float(size);
        double position_amount = str_to_float(size);

        SpPosition pos_info{nullptr};
        auto it = res.find(pair);
        if (it == res.end()) {
            pos_info = std::make_shared<Position>();
            pos_info->symbol = pair;
            pos_info->bankrupt_price = str_to_float(liquidatePrice);
            pos_info->leverage = std::stoi(std::string(leverage_text));
            pos_info->update_time = time_get_now_milli();
            res[pos_info->symbol] = pos_info;
        } else {
            pos_info = it->second;
            pos_info->update_time = time_get_now_milli();
        }

        if (side == "LONG") {
            pos_info->long_size = position_amount;
            pos_info->long_open_price = entry_price;
        } else if (side == "SHORT") {
            pos_info->short_size = position_amount;
            pos_info->short_open_price = entry_price;
        } else if (side == "BOTH") {
            if (position_amount > 0) {
                pos_info->long_size = position_amount;
                pos_info->long_open_price = entry_price;
            } else if (position_amount < 0) {
                pos_info->short_size = -position_amount;
                pos_info->short_open_price = entry_price;
            }
        }
    }
}

SpOrder parse_rtn_order(const simdjson::dom::object& obj, bool is_query) {
    std::string_view symbol = obj["symbol"];
    std::string_view clientId = obj["clientOrderId"];
    std::string_view price_sv = obj["price"];
    std::string_view qty_sv = is_query ? obj["origQty"] : obj["size"];
    std::string_view status_text = obj["status"];
    std::string orderId{};
    if (is_query) {
        orderId = std::to_string(obj["orderId"].get_int64());
    } else {
        orderId = obj["id"];
    }

    Symbol pair = transfer_to_infra_pair(symbol);
    auto rtn_order = std::make_shared<Order>(pair, ClientOrderId(clientId), OrderId(orderId));
    rtn_order->price = str_to_float(price_sv);
    rtn_order->quantity = str_to_float(qty_sv);

    std::string status_sv = to_upper_str(std::string(status_text)); // 查询对应的响应字段为小写，统一转大写进行判断
    OrderStatus order_status = OrderStatus::New;
    if (status_sv == "PENDING" || status_sv == "OPEN") {
        order_status = OrderStatus::New;
    } else if (status_sv == "PARTIALLY_FILLED") {
        order_status = OrderStatus::PartiallyFilled;
    } else if (status_sv == "FILLED") {
        order_status = OrderStatus::Filled;
    } else if (status_sv == "CANCELING") {
        order_status = OrderStatus::Canceling;
    } else if (status_sv == "CANCELED") {
        order_status = OrderStatus::Canceled;
    }
    rtn_order->status = order_status;

    if (is_query) {
        std::string_view filled_qty = obj["cumQuote"];
        std::string_view price_avg = obj["avgPrice"];
        rtn_order->cum_deal_base = str_to_float(filled_qty);
        rtn_order->avg_price = str_to_float(price_avg);
        rtn_order->cum_deal_quote = rtn_order->cum_deal_base * rtn_order->avg_price;
    } else {
        std::string_view cumFillSize = obj["cumFillSize"];
        std::string_view cumFillValue = obj["cumFillValue"];
        rtn_order->cum_deal_base = str_to_float(cumFillSize);
        rtn_order->cum_deal_quote = str_to_float(cumFillValue);
        if (cumFillSize != "0") {
            rtn_order->avg_price = rtn_order->cum_deal_quote / rtn_order->cum_deal_base;
        }
    }
    return rtn_order;
}

SpFundingFee parse_funding_fee(const simdjson::dom::element& doc) {
    simdjson::dom::array array = doc.get_array();
    for (auto item : array) {
        std::string_view symbol = item["symbol"];
        std::string_view lastFundingRate = item["lastFundingRate"];
        std::string_view forecastFundingRate = item["forecastFundingRate"];
        int64_t nextFundingTime = item["nextFundingTime"];

        Symbol pair = transfer_to_infra_pair(symbol);
        double fee = str_to_float(lastFundingRate);
        double next_fee = str_to_float(forecastFundingRate);
        return std::make_shared<FundingFee>(pair, time_get_now_milli(), fee, nextFundingTime, next_fee);
    }
    return nullptr;
}

void parse_pairs_info(const simdjson::dom::element& doc, const Currency& currency) {
    g_pairs_info_cache.clear();
    g_all_symbols.clear();
    simdjson::dom::array array = doc["symbols"];
    for (auto item : array) {
        std::string_view symbol_sv = item["symbol"];
        if (symbol_sv == "AIA_CONTRACTUSDT") { // 该合约无法订阅订单簿，也无法在网页上查看，跳过
            continue;
        }

        std::string_view quote_sv = item["quoteAsset"];
        Currency quote(quote_sv);
        if (!compare_currency(quote, currency)) {
            continue;
        }

        double minOrderSize = item["minOrderSize"];
        int64_t pricePrecision = item["pricePrecision"];
        int64_t quantityPrecision = item["quantityPrecision"];
        double contractVal = item["contractVal"];

        Symbol pair = transfer_to_infra_pair(symbol_sv);
        auto info = std::make_shared<ExchangePairInfo>();
        info->pair = pair;
        info->trading_min_base = minOrderSize;
        info->step_size_base = transfer_precision(quantityPrecision);
        info->step_size_quote = transfer_precision(pricePrecision);
        info->denomination_value = contractVal;

        g_pairs_info_cache[pair] = info;
        g_all_symbols.push_back(std::move(pair));
    }
}
} // namespace infra::weex
