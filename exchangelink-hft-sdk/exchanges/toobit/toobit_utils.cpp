#include "toobit_utils.h"

namespace infra::toobit {
double get_denomination_value(const Symbol& pair) {
    auto it = g_pairs_info_cache.find(to_lower_str(pair));
    if (it != g_pairs_info_cache.end() && it->second != nullptr) {
        return it->second->denomination_value;
    }
    return 0;
}

Errno extract_error_code(std::string_view sv) {
    if (sv.find("timeout") != std::string_view::npos) {
        return Errno::RequestTimeout;
    } else if (sv.find("API-key") != std::string_view::npos) {
        return Errno::AuthFailed;
    } else if (sv.find("insufficient") != std::string_view::npos) {
        return Errno::InsufficientBalance;
    } else if (sv.find("quantity too small") != std::string_view::npos) {
        return Errno::SmallSizeOrder;
    } else {
        return Errno::UnknownError;
    }
}

HttpRequestBody get_request_body_with_sign(boost::beast::http::verb method, const std::string& host,
                                           const std::string& path, const std::string& query,
                                           const AccountSecret& secret) {
    std::string url_str{};
    if (query.empty()) {
        url_str = path;
    } else {
        url_str = path + "?" + query;
        url_str.append("&signature=").append(generate_sign_hmac256(secret.api_secret, query));
    }

    HttpRequestBody req{method, url_str, 11};
    req.set(boost::beast::http::field::host, host);
    req.set(boost::beast::http::field::user_agent, BOOST_BEAST_VERSION_STRING);
    req.set(boost::beast::http::field::connection, "close");
    req.set("X-BB-APIKEY", secret.api_key);
    return req;
}

void parse_balance(const simdjson::dom::element& doc, const Currency& currency, UMCurrencyBalance& res) {
    res.clear();
    simdjson::dom::array array = doc.get_array();
    for (auto item : array) {
        std::string_view asset_text = item["asset"];
        std::string asset(asset_text);
        // NOTE：currency值为空时返回所有，不为空时只返回currency对应的资产
        if (!currency.empty() && !compare_currency(asset, currency)) {
            continue;
        }

        std::string_view availableBalance = item["availableBalance"];
        std::string_view positionMargin = item["positionMargin"];
        std::string_view orderMargin = item["orderMargin"];
        double available = str_to_float(availableBalance);
        double freeze = str_to_float(positionMargin) + str_to_float(orderMargin);

        auto account_asset = std::make_shared<Balance>(asset, available, freeze);
        account_asset->withdraw = available;
        res[account_asset->currency] = account_asset;
    }
}

void parse_position(const simdjson::dom::element& doc, UMSymbolPosition& res) {
    res.clear();
    simdjson::dom::array array = doc.get_array();
    for (auto item : array) {
        std::string_view symbol_text = item["symbol"];
        std::string_view avgPrice = item["avgPrice"];
        std::string_view positionSide = item["side"];
        std::string_view positionAmt_text = item["position"];
        std::string_view leverage_text = item["leverage"];
        std::string_view marginType = item["marginType"];

        Symbol pair = transfer_to_infra_pair(symbol_text);
        SpPosition pos_info{nullptr};
        auto it = res.find(pair);
        if (it == res.end()) {
            pos_info = std::make_shared<Position>();
            pos_info->position_mode = PositionMode::hedge_mode;
            pos_info->margin_mode = marginType == "CROSS" ? MarginMode::CROSS : MarginMode::ISOLATED;
            pos_info->symbol = pair;
            pos_info->leverage = std::stoi(std::string(leverage_text));
            pos_info->update_time = time_get_now_milli();
            res[pos_info->symbol] = pos_info;
        } else {
            pos_info = it->second;
            pos_info->update_time = time_get_now_milli();
        }

        double denomination = get_denomination_value(pair); // 合约张数转币数
        if (denomination == 0) {
            INFRA_LOG_WARN("[toobit] [get_position] [fail], msg: not found denomination value for {}", pair);
            continue;
        }
        double entry_price = str_to_float(avgPrice);
        double position_size = str_to_float(positionAmt_text);
        double position_amount = position_size * denomination;

        if (positionSide == "LONG") {
            pos_info->long_size = position_amount;
            pos_info->long_open_price = entry_price;
        } else if (positionSide == "SHORT") {
            pos_info->short_size = position_amount;
            pos_info->short_open_price = entry_price;
        }
    }
}

SpOrder parse_rtn_order(const simdjson::dom::object& obj, bool intact) {
    std::string_view order_id = intact ? obj["orderId"] : obj["i"];
    std::string_view symbol_text = intact ? obj["symbol"] : obj["s"];
    std::string_view client_oid_text = intact ? obj["clientOrderId"] : obj["c"];
    std::string_view order_status_text = intact ? obj["status"] : obj["X"];
    std::string_view price_text = intact ? obj["price"] : obj["p"];
    std::string_view quantity_text = intact ? obj["origQty"] : obj["q"];
    std::string_view executedQty_text = intact ? obj["executedQty"] : obj["z"];
    std::string_view avgPrice_text = intact ? obj["avgPrice"] : obj["L"];
    std::string_view create_milli = intact ? obj["time"] : obj["O"];
    std::string_view update_milli = intact ? obj["updateTime"] : obj["U"];

    std::string pair = transfer_to_infra_pair(symbol_text);
    std::string client_oid(client_oid_text);
    std::string market_oid(order_id);
    SpOrder rtn_order = std::make_shared<Order>(pair, client_oid, market_oid);

    OrderStatus order_status = to_order_status(order_status_text);
    if (order_status == OrderStatus::Expired) {
        order_status = OrderStatus::Canceled;
    }

    double denomination = get_denomination_value(pair); // 合约张数转币数
    if (denomination == 0) {
        INFRA_LOG_WARN("[toobit] [parse_rtn_order] [fail], msg: not found denomination value for {}", pair);
    }

    rtn_order->status = order_status;
    rtn_order->quantity = str_to_float(quantity_text) * denomination;
    rtn_order->price = str_to_float(price_text);
    rtn_order->avg_price = str_to_float(avgPrice_text);
    rtn_order->cum_deal_base = str_to_float(executedQty_text) * denomination;
    rtn_order->cum_deal_quote = rtn_order->cum_deal_base * rtn_order->avg_price;
    rtn_order->exchange_create_time = std::stoll(std::string(create_milli));
    rtn_order->exchange_update_time = std::stoll(std::string(update_milli));
    return rtn_order;
}

SpFundingFee parse_funding_fee(const simdjson::dom::element& doc) {
    simdjson::dom::array array = doc.get_array();
    for (auto item : array) {
        std::string_view symbol = item["symbol"];
        std::string_view rate = item["rate"];
        double fee = str_to_float(rate);
        Symbol pair = transfer_to_infra_pair(symbol);
        return std::make_shared<FundingFee>(pair, time_get_now_milli(), fee);
    }
    return nullptr;
}

void parse_pairs_info(const simdjson::dom::element& doc, const Currency& currency) {
    g_pairs_info_cache.clear();
    g_all_symbols.clear();
    simdjson::dom::array array = doc["contracts"]; // 取合约信息
    for (auto symbol_item : array) {
        std::string_view symbol_text = symbol_item["symbol"];
        std::string_view status = symbol_item["status"];
        std::string_view quoteAsset = symbol_item["quoteAsset"];
        std::string_view contractMultiplier = symbol_item["contractMultiplier"];
        std::string symbol(symbol_text);
        std::string quote(quoteAsset);

        // NOTE：过滤不开放交易的合约，以及计价币种不匹配的合约
        if (status != "TRADING" || !compare_currency(quote, currency)) {
            continue;
        }

        double trading_min_base{}, step_size_base{}, step_size_quote{}, min_notional{};
        simdjson::dom::array filters_array = symbol_item["filters"];
        for (auto item : filters_array) {
            std::string_view filter_type = item["filterType"];
            if (filter_type == "LOT_SIZE") {
                std::string_view minQty = item["minQty"];
                std::string_view stepSize = item["stepSize"];
                trading_min_base = str_to_float(minQty);
                step_size_base = str_to_float(stepSize);
            }

            if (filter_type == "PRICE_FILTER") {
                std::string_view tickSize = item["tickSize"];
                step_size_quote = str_to_float(tickSize);
            }

            if (filter_type == "MIN_NOTIONAL") {
                std::string_view notional = item["minNotional"];
                min_notional = str_to_float(notional);
            }
        }

        Symbol pair = transfer_to_infra_pair(symbol_text);
        auto pair_info = std::make_shared<ExchangePairInfo>();
        pair_info->pair = pair;
        pair_info->trading_min_base = trading_min_base;
        pair_info->step_size_base = step_size_base;
        pair_info->step_size_quote = step_size_quote;
        pair_info->min_size_quote = min_notional;
        pair_info->denomination_value = str_to_float(contractMultiplier);

        g_pairs_info_cache[pair] = pair_info;
        g_all_symbols.push_back(std::move(pair));
    }
}
} // namespace infra::toobit