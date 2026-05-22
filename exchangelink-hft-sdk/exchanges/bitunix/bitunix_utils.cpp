#include "bitunix_utils.h"

namespace infra::bitunix {
Errno extract_error_code(std::string_view sv) {
    if (sv.find("timeout") != std::string_view::npos) {
        return Errno::RequestTimeout;
    } else if (sv.find("API-key") != std::string_view::npos) {
        return Errno::AuthFailed;
    } else if (sv.find("Insufficient") != std::string_view::npos) {
        return Errno::InsufficientBalance;
    } else if (sv.find("less than minimum buy price") != std::string_view::npos) {
        return Errno::SmallSizeOrder;
    } else if (sv.find("amount should be larger than") != std::string_view::npos) {
        return Errno::SmallSizeOrder;
    } else {
        return Errno::UnknownError;
    }
}

HttpRequestBody get_request_body_with_sign(boost::beast::http::verb method, const std::string& host,
                                           const std::string& path, const std::string& query, const std::string& body,
                                           const AccountSecret& secret) {
    std::string nonce = get_random_str(32);
    std::string timestamp = std::to_string(time_get_now_milli());
    std::string query_param{};
    if (!query.empty()) {
        for (char c : query) {
            if (c != '=') {
                query_param.push_back(c);
            }
        }
    }

    std::string digest_input = nonce + timestamp + secret.api_key + query_param + body;
    std::string digest = generate_hash_sha256(digest_input);
    std::string sign_input = digest + secret.api_secret;
    std::string signature = generate_hash_sha256(sign_input);

    using namespace boost::beast;
    std::string url_str{};
    if (method == http::verb::get) {
        url_str = query.empty() ? path : (path + "?" + query);
    } else if (method == http::verb::post) {
        url_str = path;
    }

    HttpRequestBody req{method, url_str, 11};
    req.set(http::field::host, host);
    req.set(http::field::user_agent, BOOST_BEAST_VERSION_STRING);
    req.set(http::field::connection, "close");

    req.set("api-key", secret.api_key);
    req.set("nonce", nonce);
    req.set("timestamp", timestamp);
    req.set("sign", signature);

    if (method == http::verb::post) {
        req.set(http::field::content_type, "application/json");
        req.body() = body;
        req.prepare_payload();
    }
    return req;
}

void parse_balance(const simdjson::dom::element& doc, const Currency& currency, UMCurrencyBalance& res) {
    res.clear();
    simdjson::dom::object item = doc["data"];
    std::string_view marginCoin = item["marginCoin"];
    std::string_view available = item["available"];
    std::string_view frozen = item["frozen"];
    std::string_view margin = item["margin"];
    std::string_view crossUnrealizedPNL = item["crossUnrealizedPNL"];
    std::string_view transfer = item["transfer"];

    std::string asset(marginCoin);
    double available_v = str_to_float(available) + str_to_float(crossUnrealizedPNL);
    double frozen_v = str_to_float(frozen) + str_to_float(margin);
    auto account_asset = std::make_shared<Balance>(asset, available_v, frozen_v);
    account_asset->withdraw = str_to_float(transfer);
    res[asset] = account_asset;
}

void parse_position(const simdjson::dom::element& doc, UMSymbolPosition& res) {
    res.clear();
    simdjson::dom::array array = doc["data"];
    for (auto item : array) {
        std::string_view symbol = item["symbol"];
        std::string_view avgOpenPrice = item["avgOpenPrice"];
        std::string_view qty = item["qty"];
        std::string_view positionMode = item["positionMode"];
        std::string_view marginMode = item["marginMode"];
        int64_t leverage = item["leverage"];

        Symbol pair = transfer_to_infra_pair(symbol);
        SpPosition pos_info{nullptr};
        auto it = res.find(pair);
        if (it == res.end()) {
            pos_info = std::make_shared<Position>();
            pos_info->position_mode =
                (positionMode == "ONE_WAY") ? PositionMode::one_way_mode : PositionMode::hedge_mode;
            pos_info->margin_mode = (marginMode == "CROSS") ? MarginMode::CROSS : MarginMode::ISOLATED;
            pos_info->symbol = pair;
            // pos_info->bankrupt_price = str_to_float(liqPrice);
            pos_info->leverage = leverage;
            pos_info->update_time = time_get_now_milli();
            res[pos_info->symbol] = pos_info;
        } else {
            pos_info = it->second;
            pos_info->update_time = time_get_now_milli();
        }

        double entry_price = str_to_float(avgOpenPrice);
        double position_amount = str_to_float(qty);

        if (positionMode == "HEDGE") {
            pos_info->long_size = position_amount;
            pos_info->long_open_price = entry_price;
        } else if (positionMode == "short") {
            pos_info->short_size = position_amount;
            pos_info->short_open_price = entry_price;
        } else if (positionMode == "ONE_WAY") {
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
    std::string_view clientId = obj["clientId"];
    std::string_view orderId = obj["orderId"];
    std::string_view price_text = obj["price"];
    std::string_view quantity_text = obj["qty"];
    std::string_view ctime = obj["ctime"];
    std::string_view mtime = obj["mtime"];

    Symbol pair = transfer_to_infra_pair(symbol);
    ClientOrderId client_oid(clientId);
    OrderId market_oid(orderId);
    auto rtn_order = std::make_shared<Order>(pair, client_oid, market_oid);
    rtn_order->price = str_to_float(price_text);
    rtn_order->quantity = str_to_float(quantity_text);

    if (is_query) {
        std::string_view orderStatus = obj["status"];
        // std::string_view dealAmount = obj["tradeQty"];
        // std::string_view averagePrice = obj["avgPrice"];
        rtn_order->status = to_order_status(orderStatus);
        // rtn_order->avg_price = str_to_float(averagePrice);
        // rtn_order->cum_deal_base = str_to_float(dealAmount);
        // rtn_order->cum_deal_quote = rtn_order->cum_deal_base * rtn_order->avg_price;
        rtn_order->exchange_create_time = std::stoul(std::string(ctime));
        rtn_order->exchange_update_time = std::stoul(std::string(mtime));
    } else {
        std::string_view orderStatus = obj["orderStatus"];
        std::string_view dealAmount = obj["dealAmount"];
        std::string_view averagePrice = obj["averagePrice"];
        rtn_order->status = to_order_status(orderStatus);
        rtn_order->avg_price = str_to_float(averagePrice);
        rtn_order->cum_deal_base = str_to_float(dealAmount);
        rtn_order->cum_deal_quote = rtn_order->cum_deal_base * rtn_order->avg_price;
        rtn_order->exchange_create_time = time_iso_to_milli(ctime.data());
        rtn_order->exchange_update_time = time_iso_to_milli(mtime.data());
    }
    return rtn_order;
}

SpFundingFee parse_funding_fee(const simdjson::dom::element& doc) {
    simdjson::dom::object obj = doc["data"];
    std::string_view symbol = obj["symbol"];
    std::string_view fundingRate = obj["fundingRate"];

    double fee = str_to_float(fundingRate);
    Symbol pair = transfer_to_infra_pair(symbol);
    return std::make_shared<FundingFee>(pair, time_get_now_milli(), fee, 0, 0);
}

double parse_margin_ratio(const simdjson::dom::element& doc) {
    simdjson::dom::object item = doc["data"];
    double available = str_to_float(item["available"]);
    double frozen = str_to_float(item["frozen"]);
    double margin = str_to_float(item["margin"]);
    double cross_unrealized_pnl = str_to_float(item["crossUnrealizedPNL"]);
    if (margin <= 0.0) {
        return 999.0;
    }
    return (available + frozen + cross_unrealized_pnl) / margin;
}

void parse_pairs_info(const simdjson::dom::element& doc, const Currency& currency) {
    g_pairs_info_cache.clear();
    g_all_symbols.clear();
    simdjson::dom::array array = doc["data"];
    for (auto item : array) {
        std::string_view symbol_text = item["symbol"];
        std::string_view symbolStatus = item["symbolStatus"];
        std::string_view quote_text = item["quote"];

        // NOTE：过滤不开放交易的合约，以及计价币种不匹配的合约
        Currency quote(quote_text);
        if (symbolStatus != "OPEN" || !compare_currency(quote, currency)) {
            continue;
        }

        std::string_view minTradeVolume = item["minTradeVolume"];
        std::int64_t basePrecision = item["basePrecision"];
        std::int64_t quotePrecision = item["quotePrecision"];

        Symbol pair = transfer_to_infra_pair(symbol_text);
        auto info = std::make_shared<ExchangePairInfo>();
        info->pair = pair;
        info->trading_min_base = str_to_float(minTradeVolume);
        info->step_size_base = get_step_by_decimals(basePrecision);
        info->step_size_quote = get_step_by_decimals(quotePrecision);

        g_pairs_info_cache[pair] = info;
        g_all_symbols.push_back(std::move(pair));
    }
}
} // namespace infra::bitunix