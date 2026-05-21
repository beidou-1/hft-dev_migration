#include "phemex_utils.h"

namespace infra::phemex {
Errno extract_error_code(std::string_view sv) {
    if (sv.find("timeout") != std::string_view::npos) {
        return Errno::RequestTimeout;
    } else if (sv.find("API-key") != std::string_view::npos) {
        return Errno::AuthFailed;
    } else if (sv.find("TE_NO_ENOUGH_AVAILABLE_BALANCE") != std::string_view::npos) {
        return Errno::InsufficientBalance;
    } else if (sv.find("OM_ORDER_NOT_FOUND") != std::string_view::npos) {
        return Errno::OrderNotFound;
    } else if (sv.find("TE_PRICE_TOO_SMALL") != std::string_view::npos) {
        return Errno::SmallSizeOrder;
    } else {
        return Errno::UnknownError;
    }
}

HttpRequestBody get_request_body_with_sign(boost::beast::http::verb method, const std::string& host,
                                           const std::string& path, const std::string& query,
                                           const AccountSecret& secret) {
    std::string url_str{};
    std::string request_body = query;
    using namespace boost::beast;
    if (method == http::verb::post) {
        url_str = path;
    } else {
        url_str = query.empty() ? path : (path + "?" + query);
    }

    std::string raw_str{};
    std::string query_text = query;
    std::string expire_second = std::to_string(time_get_now_sec() + 60);
    if (method == http::verb::post) {
        query_text = expire_second + query_text;
    } else {
        query_text = query_text + expire_second;
    }
    raw_str.append(path);
    if (!query.empty()) {
        raw_str.append(query_text);
    }
    std::string signature = generate_sign_hmac256(secret.api_secret, raw_str);
    std::transform(signature.begin(), signature.end(), signature.begin(), ::tolower);
    HttpRequestBody req{method, url_str, 11};
    req.set(http::field::host, host);
    req.set(http::field::user_agent, BOOST_BEAST_VERSION_STRING);
    req.set(http::field::connection, "close");

    req.set("x-phemex-access-token", secret.api_key);
    req.set("x-phemex-request-signature", signature);
    req.set("x-phemex-request-expiry", expire_second);

    if (method == http::verb::post) {
        req.set(http::field::content_type, "application/json");
        req.body() = request_body;
        req.prepare_payload();
    } else {
        req.set(http::field::content_type, "application/x-www-form-urlencoded");
    }
    return req;
}

void parse_balance(const simdjson::dom::element& doc, const Currency& currency, UMCurrencyBalance& res) {
    simdjson::dom::object data = doc["data"];
    simdjson::dom::object account = data["account"];
    std::string_view currency_text = account["currency"];
    std::string asset(currency_text);
    if (!currency.empty() && !compare_currency(asset, currency)) {
        return;
    }
    std::string_view total_text = account["accountBalanceRv"];
    std::string_view frozen_text = account["totalUsedBalanceRv"];
    double total = str_to_float(total_text);
    double frozen = str_to_float(frozen_text);
    auto account_asset = std::make_shared<Balance>(asset, total - frozen, frozen);
    account_asset->withdraw = account_asset->available;
    res[account_asset->currency] = account_asset;
}

void parse_position(const simdjson::dom::element& doc, UMSymbolPosition& res) {
    simdjson::dom::object data = doc["data"];
    simdjson::dom::array position_array = data["positions"];
    for (auto item : position_array) {
        std::string_view symbol_text = item["symbol"];
        std::string_view entryPrice_text = item["avgEntryPrice"];
        std::string_view positionAmt_text = item["size"];
        std::string_view positionSide_text = item["posSide"];
        std::string_view positionMode_text = item["posMode"];
        std::string_view liquidationPrice_text = item["liquidationPriceRp"];
        std::string_view leverage_text = item["leverageRr"];
        if (positionAmt_text == "0" || positionAmt_text.empty() || entryPrice_text.empty()) {
            continue;
        }
        Symbol pair = transfer_to_infra_pair(symbol_text);
        SpPosition pos_info{nullptr};
        auto it = res.find(pair);
        if (it == res.end()) {
            pos_info = std::make_shared<Position>();
            pos_info->position_mode =
                (positionMode_text == "Hedged") ? PositionMode::hedge_mode : PositionMode::one_way_mode;
            int lever = std::stoi(std::string(leverage_text));
            pos_info->leverage = abs(lever);
            pos_info->margin_mode = lever < 0 ? MarginMode::CROSS : MarginMode::ISOLATED;
            pos_info->symbol = pair;
            pos_info->bankrupt_price = str_to_float(liquidationPrice_text);
            pos_info->update_time = time_get_now_milli();
            res[pos_info->symbol] = pos_info;
        } else {
            pos_info = it->second;
            pos_info->update_time = time_get_now_milli();
        }

        double entry_price = str_to_float(entryPrice_text);
        double position_size = str_to_float(positionAmt_text);
        double position_amount = position_size;

        if (positionSide_text == "Long") {
            pos_info->long_size = position_amount;
            pos_info->long_open_price = entry_price;
        } else if (positionSide_text == "Short") {
            pos_info->short_size = position_amount;
            pos_info->short_open_price = entry_price;
        } else if (positionSide_text == "Merged") {
            if (position_amount > 0) {
                pos_info->long_size = position_amount;
                pos_info->long_open_price = entry_price;
            } else if (position_amount < 0) {
                pos_info->short_size = -position_amount; // 取正数
                pos_info->short_open_price = entry_price;
            }
        }
    }
}

SpOrder parse_rtn_order(const simdjson::dom::object& obj, bool is_rest) {
    std::string_view symbol = obj["symbol"];
    std::string_view client_order_id = is_rest ? obj["clOrdId"] : obj["clOrdID"];
    std::string_view order_id = is_rest ? obj["orderId"] : obj["orderID"];
    std::string_view price = obj["priceRp"];
    std::string_view size = is_rest ? obj["orderQtyRq"] : obj["orderQty"];
    std::string_view deal_size = is_rest ? obj["cumQtyRq"] : obj["cumQty"];
    std::string_view deal_value = obj["cumValueRv"];
    std::string_view order_status_text = obj["ordStatus"];
    Timestamp create_milli = obj["actionTimeNs"].get_int64() / 1000000;
    Timestamp update_milli = obj["transactTimeNs"].get_int64() / 1000000;
    Symbol pair = transfer_to_infra_pair(symbol);
    ClientOrderId client_oid(client_order_id);
    OrderId market_oid(order_id);
    auto rtn_order = std::make_shared<Order>(pair, client_oid, market_oid);

    double filled_qty = str_to_float(deal_size);
    double filled_value = str_to_float(deal_value);
    double deal_avg_price = (filled_qty > 0.0) ? (filled_value / filled_qty) : double(0.0);
    std::string status_text(order_status_text);
    if (status_text == "PartiallyFilled") {
        status_text = "partially_filled";
    } else if (status_text == "Created" || status_text == "Init" || status_text == "Untriggered" ||
               status_text == "Triggered") {
        status_text = "live";
    }
    std::transform(status_text.begin(), status_text.end(), status_text.begin(), ::toupper);
    OrderStatus order_status = to_order_status(status_text);

    rtn_order->status = order_status;
    rtn_order->price = str_to_float(price);
    rtn_order->quantity = str_to_float(size);
    rtn_order->avg_price = deal_avg_price;
    rtn_order->cum_deal_base = filled_qty;
    rtn_order->cum_deal_quote = filled_value;
    rtn_order->exchange_create_time = create_milli;
    rtn_order->exchange_update_time = update_milli;
    return rtn_order;
}

SpFundingFee parse_funding_fee(const simdjson::dom::element& doc) {
    simdjson::dom::object result = doc["result"];
    std::string_view pair = result["symbol"];
    std::string_view fee_text = result["fundingRateRr"];
    int64_t update_time = result["timestamp"].get_int64() / 1000000;
    double fee = str_to_float(fee_text);
    Timestamp milli = time_get_now_milli();
    Timestamp next_milli = update_time;
    return std::make_shared<FundingFee>(transfer_to_infra_pair(pair), milli, fee, next_milli, 0);
}

void parse_pairs_info(const simdjson::dom::element& doc, const Currency& currency) {
    g_pairs_info_cache.clear();
    g_all_symbols.clear();
    simdjson::dom::object data = doc["data"];
    simdjson::dom::array list = data["perpProductsV2"];
    for (auto item : list) {
        std::string_view symbol_text = item["symbol"];
        std::string_view qtyStepSize_text = item["qtyStepSize"];
        std::string_view priceStepSize_text = item["tickSize"];
        std::string_view settleCcy = item["quoteCurrency"];
        std::string_view status = item["status"];
        Currency quote(settleCcy);
        if (status != "Listed" || !compare_currency(quote, currency))
            continue;

        std::string pair = transfer_to_infra_pair(symbol_text);
        double trading_min_base = str_to_float(qtyStepSize_text);
        double step_size_base = trading_min_base;
        double step_size_quote = str_to_float(priceStepSize_text);
        SpExPairInfo pair_info = std::make_shared<ExchangePairInfo>();
        pair_info->pair = pair;
        pair_info->trading_min_base = trading_min_base;
        pair_info->step_size_base = step_size_base;
        pair_info->step_size_quote = step_size_quote;
        pair_info->denomination_value = 1;
        g_pairs_info_cache[pair] = pair_info;
        g_all_symbols.push_back(std::move(pair));
    }
}
} // namespace infra::phemex