#include "bybit_utils.h"

namespace infra::bybit {
Currency get_right_currency(const Currency& currency) {
    std::string str = currency;
    std::transform(str.begin(), str.end(), str.begin(), ::toupper);
    return str;
}

Errno extract_error_msg(std::string_view sv) {
    if (sv.find("timeout") != std::string_view::npos) {
        return Errno::RequestTimeout;
    } else if (sv.find("API key") != std::string_view::npos || sv.find("invalid API key") != std::string_view::npos) {
        return Errno::AuthFailed;
    } else if (sv.find("insufficient") != std::string_view::npos) {
        return Errno::InsufficientBalance;
    } else if (sv.find("order not exists") != std::string_view::npos ||
               sv.find("invalid order") != std::string_view::npos) {
        return Errno::OrderNotFound;
    } else if (sv.find("duplicated") != std::string_view::npos || sv.find("Duplicate") != std::string_view::npos) {
        return Errno::DuplicatedId;
    } else if (sv.find("not meet minimum order value") != std::string_view::npos) {
        return Errno::SmallSizeOrder;
    } else {
        return Errno::UnknownError;
    }
}

HttpRequestBody get_request_body_with_sign(boost::beast::http::verb method, const std::string& host,
                                           const std::string& path, const std::string& query,
                                           const AccountSecret& secret) {
    std::string url_str{};
    std::string request_body{};
    std::string timestamp = std::to_string(time_get_now_milli());
    std::string recv_window = "5000";
    std::string sign_data = timestamp + secret.api_key + recv_window;
    using namespace boost::beast;
    if (method == http::verb::get || method == http::verb::delete_) {
        sign_data += query;
        url_str = query.empty() ? path : (path + "?" + query);
    } else {
        request_body = query;
        sign_data += request_body;
        url_str = path;
    }

    std::string signature = generate_sign_hmac256(secret.api_secret, sign_data);

    HttpRequestBody req{method, url_str, 11};
    req.set(http::field::host, host);
    req.set(http::field::user_agent, BOOST_BEAST_VERSION_STRING);
    req.set(http::field::connection, "close");

    req.set("X-BAPI-API-KEY", secret.api_key);
    req.set("X-BAPI-TIMESTAMP", timestamp);
    req.set("X-BAPI-RECV-WINDOW", recv_window);
    req.set("X-BAPI-SIGN", signature);

    if (method == http::verb::post) {
        req.set(http::field::content_type, "application/json");
        req.body() = request_body;
        req.prepare_payload();
    }
    return req;
}

void parse_balance(const simdjson::dom::element& doc, const Currency& currency, UMCurrencyBalance& res) {
    res.clear();
    simdjson::dom::array array = doc["result"]["list"];
    for (auto account_item : array) {
        simdjson::dom::array coin_list = account_item["coin"];
        for (auto item : coin_list) {
            std::string_view coin_text = item["coin"];
            std::string asset(coin_text);
            // NOTE：currency值为空时返回所有，不为空时只返回currency对应的资产
            if (!currency.empty() && !compare_currency(asset, currency)) {
                continue;
            }

            std::string_view walletBalance = item["walletBalance"];
            std::string_view totalOrderIM = item["totalOrderIM"];
            std::string_view totalPositionIM = item["totalPositionIM"];
            std::string_view cumRealisedPnl = item["cumRealisedPnl"];
            std::string_view borrowAmount = item["borrowAmount"];

            bfloat equity = str_to_float(walletBalance);
            bfloat frozen = str_to_float(totalOrderIM) + str_to_float(totalPositionIM);
            auto account_asset = std::make_shared<Balance>(asset, equity - frozen, frozen);
            account_asset->realised_pnl = str_to_float(cumRealisedPnl);
            account_asset->borrow = str_to_float(borrowAmount);
            res[account_asset->currency] = account_asset;
        }
    }
}

void parse_position(const simdjson::dom::element& doc, UMSymbolPosition& res) {
    res.clear();
    simdjson::dom::array array = doc["result"]["list"];
    for (auto item : array) {
        std::string_view symbol_text = item["symbol"];
        std::string_view entryPrice_text = item["avgPrice"];
        std::string_view side_text = item["side"];
        int64_t positionIdx = item["positionIdx"];
        std::string_view positionAmt_text = item["size"];
        std::string_view liqPrice = item["liqPrice"];
        std::string_view leverage_text = item["leverage"];

        std::string pair = transfer_to_infra_pair(symbol_text);
        bfloat entry_price = str_to_float(entryPrice_text);
        bfloat position_amount = str_to_float(positionAmt_text);

        SpPosition pos_info{nullptr};
        auto it = res.find(pair);
        if (it == res.end()) {
            pos_info = std::make_shared<Position>();
            pos_info->position_mode = (positionIdx == 0) ? PositionMode::one_way_mode : PositionMode::hedge_mode;
            pos_info->symbol = pair;
            pos_info->bankrupt_price = str_to_float(liqPrice);
            pos_info->leverage = std::stoi(std::string(leverage_text));
            pos_info->update_time = time_get_now_milli();
            res[pos_info->symbol] = pos_info;
        } else {
            pos_info = it->second;
            pos_info->update_time = time_get_now_milli();
        }

        if (positionIdx == 1) {
            pos_info->long_size = position_amount;
            pos_info->long_open_price = entry_price;
        } else if (positionIdx == 2) {
            pos_info->short_size = -position_amount; // 取正数
            pos_info->short_open_price = entry_price;
        } else if (positionIdx == 0) {
            if (side_text == "Buy") {
                pos_info->long_size = position_amount;
                pos_info->long_open_price = entry_price;
            } else if (side_text == "Sell") {
                pos_info->short_size = -position_amount; // 取正数
                pos_info->short_open_price = entry_price;
            }
        }
    }
}

SpOrder parse_rtn_order(const simdjson::dom::object& obj) {
    std::string_view symbol = obj["symbol"];
    std::string_view orderLinkId = obj["orderLinkId"];
    std::string_view orderId = obj["orderId"];
    std::string_view orderStatus = obj["orderStatus"];
    std::string_view price_text = obj["price"];
    std::string_view quantity_text = obj["qty"];
    std::string_view cumExecQty = obj["cumExecQty"];
    std::string_view avgPrice = obj["avgPrice"];
    std::string_view createdTime = obj["createdTime"];
    std::string_view updatedTime = obj["updatedTime"];

    Symbol pair = transfer_to_infra_pair(symbol);
    ClientOrderId client_oid(orderLinkId);
    OrderId market_oid(orderId);
    auto rtn_order = std::make_shared<Order>(pair, client_oid, market_oid);

    OrderStatus order_status = OrderStatus::New;
    if (orderStatus == "New")
        order_status = OrderStatus::New;
    else if (orderStatus == "PartiallyFilled")
        order_status = OrderStatus::PartiallyFilled;
    else if (orderStatus == "Filled")
        order_status = OrderStatus::Filled;
    else if (orderStatus == "Cancelled")
        order_status = OrderStatus::Canceled;
    else if (orderStatus == "Rejected")
        order_status = OrderStatus::Rejected;
    else if (orderStatus == "Untriggered")
        order_status = OrderStatus::New;
    else if (orderStatus == "Deactivated")
        order_status = OrderStatus::Canceled;
    else if (orderStatus == "Triggered")
        order_status = OrderStatus::New;

    rtn_order->status = order_status;
    rtn_order->price = str_to_float(price_text);
    rtn_order->quantity = str_to_float(quantity_text);
    rtn_order->avg_price = str_to_float(avgPrice);
    rtn_order->cum_deal_base = str_to_float(cumExecQty);

    rtn_order->cum_deal_quote = rtn_order->cum_deal_base * rtn_order->avg_price;
    rtn_order->exchange_create_time = std::stoll(std::string(createdTime));
    rtn_order->exchange_update_time = std::stoll(std::string(updatedTime));
    return rtn_order;
}

SpFundingFee parse_funding_fee(const simdjson::dom::element& doc) {
    simdjson::dom::array array = doc["result"]["list"];
    for (auto obj : array) {
        std::string_view symbol = obj["symbol"];
        std::string_view fundingRate = obj["fundingRate"];
        std::string_view fundingRateTimestamp = obj["fundingRateTimestamp"];

        Symbol pair = to_infra_pair(Exchange::BYBIT, symbol);
        bfloat fee = str_to_float(fundingRate);
        Timestamp next_milli = std::stoll(std::string(fundingRateTimestamp)) + 8 * 60 * 60 * 1000;
        return std::make_shared<FundingFee>(pair, time_get_now_milli(), fee, next_milli, 0);
    }
    return nullptr;
}

void parse_pairs_info(const simdjson::dom::element& doc, const Currency& currency) {
    g_pairs_info_cache.clear();
    g_all_symbols.clear();
    simdjson::dom::array array = doc["result"]["list"];
    for (auto item : array) {
        std::string_view symbol_text = item["symbol"];
        std::string_view contractType = item["contractType"];
        std::string_view status = item["status"];
        std::string_view quoteCoin = item["quoteCoin"];

        if (contractType != "LinearPerpetual") {
            continue;
        }

        // NOTE：过滤不开放交易的合约，以及计价币种不匹配的合约
        Currency quote(quoteCoin);
        if (status != "Trading" || !compare_currency(quote, currency)) {
            continue;
        }

        // NOTE：转成统一格式 base-quote
        Symbol pair = transfer_to_infra_pair(symbol_text);

        // --- 提取过滤器参数 ---
        bfloat trading_min_base{}, step_size_base{}, step_size_quote{}, min_notional{};

        // 1. 数量限制 (lotSizeFilter)
        simdjson::dom::element lot_size_filter;
        if (item["lotSizeFilter"].get(lot_size_filter) == simdjson::SUCCESS) {
            std::string_view minOrderQty = lot_size_filter["minOrderQty"];
            std::string_view qtyStep = lot_size_filter["qtyStep"];
            // 现货可能有 minNotionalValue，合约通常没有(或通过 minQty*Price 计算)
            std::string_view minNotionalValue;
            trading_min_base = str_to_float(minOrderQty);
            step_size_base = str_to_float(qtyStep);
            if (lot_size_filter["minNotionalValue"].get(minNotionalValue) == simdjson::SUCCESS) {
                min_notional = str_to_float(minNotionalValue);
            }
        }

        simdjson::dom::element price_filter;
        if (item["priceFilter"].get(price_filter) == simdjson::SUCCESS) {
            std::string_view tickSize = price_filter["tickSize"];
            step_size_quote = str_to_float(tickSize);
        }

        SpExPairInfo pair_info = std::make_shared<ExchangePairInfo>();
        pair_info->pair = pair;
        pair_info->trading_min_base = trading_min_base;
        pair_info->step_size_base = step_size_base;
        pair_info->step_size_quote = step_size_quote;
        pair_info->min_size_quote = min_notional;

        g_pairs_info_cache[pair] = pair_info;
        g_all_symbols.push_back(std::move(pair));
    }
}
} // namespace infra::bybit