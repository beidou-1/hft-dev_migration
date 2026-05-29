#include "kucoin_utils.h"
#include <regex>

namespace infra::kucoin {
double get_denomination_value(const Symbol& pair) {
    auto it = g_pairs_info_cache.find(pair);
    if (it != g_pairs_info_cache.end() && it->second != nullptr) {
        return it->second->denomination_value;
    }
    return 0;
}

Currency get_right_currency(const Currency& currency) {
    std::string str = currency;
    std::transform(str.begin(), str.end(), str.begin(), ::toupper);
    return str;
}

Errno extract_error_code(std::string_view sv) {
    if (sv.find("timeout") != std::string_view::npos) {
        return Errno::RequestTimeout;
    } else if (sv.find("API-key") != std::string_view::npos) {
        return Errno::AuthFailed;
    } else if (sv.find("Insufficient funds") != std::string_view::npos) {
        return Errno::InsufficientBalance;
    } else if (sv.find("insufficient available margin") != std::string_view::npos) {
        return Errno::InsufficientBalance;
    } else if (sv.find("orderNotExist") != std::string_view::npos) {
        return Errno::OrderNotFound;
    } else if (sv.find("Order ID not found") != std::string_view::npos) {
        return Errno::OrderNotFound;
    } else if (sv.find("quantity parameter is invalid") != std::string_view::npos) {
        return Errno::SmallSizeOrder;
    } else if (sv.find("No positions available to close/reduce") != std::string_view::npos) {
        return Errno::ReduceOnlyRejected;
    } else {
        return Errno::UnknownError;
    }
}

HttpRequestBody get_request_body_with_sign(boost::beast::http::verb method, const std::string& host,
                                           const std::string& path, const std::string& query,
                                           const AccountSecret& secret) {
    std::string url_str{};
    std::string request_body;

    if (method == boost::beast::http::verb::get or method == boost::beast::http::verb::delete_) {
        url_str = query.empty() ? path : (path + "?" + query);
    } else if (method == boost::beast::http::verb::post) {
        url_str = path;
        request_body = query;
    }

    std::string raw_str{};
    std::string timestamp = std::to_string(time_get_now_milli());
    std::string method_str;
    if (method == boost::beast::http::verb::get) {
        method_str = "GET";
    } else if (method == boost::beast::http::verb::post) {
        method_str = "POST";
    } else if (method == boost::beast::http::verb::delete_) {
        method_str = "DELETE";
    }
    raw_str.append(timestamp).append(method_str).append(url_str).append(request_body);

    std::string signature = generate_sign_hmac256_b64(secret.api_secret, raw_str);
    std::string passphrase = generate_sign_hmac256_b64(secret.api_secret, secret.api_phrase);

    HttpRequestBody req{method, url_str, 11};
    using namespace boost::beast;
    req.set(http::field::host, host);
    req.set(http::field::user_agent, BOOST_BEAST_VERSION_STRING);

    req.set("KC-API-KEY", secret.api_key);
    req.set("KC-API-SIGN", signature);
    req.set("KC-API-TIMESTAMP", timestamp);
    req.set("KC-API-PASSPHRASE", passphrase);
    req.set("KC-API-KEY-VERSION", "2");
    if (not request_body.empty()) {
        req.set(http::field::content_type, "application/json");
        req.body() = query;
        req.prepare_payload();
    }
    return req;
}

double parse_classic_margin_ratio(const simdjson::dom::element& doc) {
    simdjson::dom::object data = doc["data"];
    return data["riskRatio"].get_double();
}

double parse_unified_margin_ratio(const simdjson::dom::element& doc) {
    simdjson::dom::array accounts = doc["data"]["accounts"];
    if (accounts.size() == 0)
        return 999.0;
    simdjson::dom::array currencies = (*accounts.begin())["currencies"];
    double total_equity = 0.0;
    double total_margin = 0.0;
    for (auto item : currencies) {
        double equity = str_to_float(item["equity"]);
        double hold = str_to_float(item["hold"]);
        double liability = str_to_float(item["liability"]);
        total_equity += equity;
        total_margin += hold + liability;
    }
    if (total_margin <= 0.0) {
        return 999.0;
    }
    return total_equity / total_margin;
}

void parse_classic_balance(const simdjson::dom::element& doc, const Currency& currency, UMCurrencyBalance& res) {
    res.clear();
    simdjson::dom::object obj = doc["data"];
    std::string_view currency_text = obj["currency"];
    std::string asset(currency_text);
    // NOTE：currency值为空时返回所有，不为空时只返回currency对应的资产
    if (!currency.empty() && !compare_currency(asset, currency)) {
        return;
    }

    double availableBalance = obj["availableBalance"].get_double();
    double accountEquity = obj["accountEquity"].get_double();

    auto balance_info = std::make_shared<Balance>(asset, availableBalance, accountEquity - availableBalance);
    balance_info->withdraw = balance_info->available;
    res[balance_info->currency] = balance_info;
}

void parse_unified_balance(const simdjson::dom::element& doc, const Currency& currency, UMCurrencyBalance& res) {
    res.clear();
    simdjson::dom::array accounts = doc["data"]["accounts"];
    if (accounts.size() == 0)
        return;
    simdjson::dom::array array = (*accounts.begin())["currencies"];

    for (auto obj : array) {
        std::string_view currency_text = obj["currency"];
        std::string asset(currency_text);
        // NOTE：currency值为空时返回所有，不为空时只返回currency对应的资产
        if (!currency.empty() && !compare_currency(asset, currency)) {
            continue;
        }

        std::string_view available_str = obj["available"];
        std::string_view hold_str = obj["hold"];
        double available = str_to_float(available_str);
        double hold = str_to_float(hold_str);

        auto balance_info = std::make_shared<Balance>(asset, available, hold);
        balance_info->withdraw = balance_info->available;
        res[balance_info->currency] = balance_info;
    }
}

void parse_classic_position_object(const simdjson::dom::object& obj, const Symbol& symbol, UMSymbolPosition& res) {
    std::string_view symbol_text = obj["symbol"];
    std::string pair = transfer_to_infra_pair(symbol_text);
    // NOTE：symbol值为空时返回所有，不为空时只返回symbol对应的持仓
    if (!symbol.empty() && pair != symbol) {
        return;
    }

    std::string_view margin_mode_text = obj["marginMode"];
    double avg_entry_price = obj["avgEntryPrice"];
    std::string_view positionSide_text = obj["positionSide"];
    std::int64_t current_qty = obj["currentQty"];
    double bankrupt_price = obj["bankruptPrice"];
    double leverage = obj["leverage"].error() == simdjson::SUCCESS ? obj["leverage"] : 0.0;

    SpPosition pos_info{nullptr};
    auto it = res.find(pair);
    if (it == res.end()) {
        pos_info = std::make_shared<Position>();
        pos_info->position_mode = (positionSide_text == "BOTH") ? PositionMode::one_way_mode : PositionMode::hedge_mode;
        pos_info->margin_mode = (margin_mode_text == "ISOLATED") ? MarginMode::ISOLATED : MarginMode::CROSS;
        pos_info->symbol = pair;
        pos_info->bankrupt_price = bankrupt_price;
        pos_info->leverage = leverage;
        pos_info->update_time = time_get_now_milli();
        res[pos_info->symbol] = pos_info;
    } else {
        pos_info = it->second;
        pos_info->update_time = time_get_now_milli();
    }

    double denomination = get_denomination_value(pair); // 合约张数转币数
    if (denomination == 0) {
        INFRA_LOG_WARN("[kucoin] [get_position] [fail], msg: not found denomination value for {}", pair);
        return;
    }

    double entry_price = avg_entry_price;
    double position_amount = current_qty;
    double position_size = position_amount * denomination;

    if (positionSide_text == "LONG") {
        pos_info->long_size = position_size;
        pos_info->long_open_price = entry_price;
    } else if (positionSide_text == "SHORT") {
        pos_info->short_size = -position_size; // 取正数
        pos_info->short_open_price = entry_price;
    } else if (positionSide_text == "BOTH") {
        if (position_size > 0) {
            pos_info->long_size = position_size;
            pos_info->long_open_price = entry_price;
        } else if (position_size < 0) {
            pos_info->short_size = -position_size; // 取正数
            pos_info->short_open_price = entry_price;
        }
    }
}

void parse_unified_position_object(const simdjson::dom::object& obj, const Symbol& symbol, UMSymbolPosition& res) {
    std::string_view symbol_text = obj["symbol"];
    std::string pair = transfer_to_infra_pair(symbol_text);
    // NOTE：symbol值为空时返回所有，不为空时只返回symbol对应的持仓
    if (!symbol.empty() && pair != symbol) {
        return;
    }

    std::string_view margin_mode_text = obj["marginMode"];
    std::string_view entry_price_text = obj["entryPrice"];
    std::string_view size_text = obj["size"];
    std::string_view leverage = obj["leverage"];

    SpPosition pos_info{nullptr};
    auto it = res.find(pair);
    if (it == res.end()) {
        pos_info = std::make_shared<Position>();
        pos_info->margin_mode = (margin_mode_text == "ISOLATED") ? MarginMode::ISOLATED : MarginMode::CROSS;
        pos_info->symbol = pair;
        pos_info->leverage = std::stoi(std::string(leverage));
        pos_info->update_time = time_get_now_milli();
        res[pos_info->symbol] = pos_info;
    } else {
        pos_info = it->second;
        pos_info->update_time = time_get_now_milli();
    }

    double denomination = get_denomination_value(pair); // 合约张数转币数
    if (denomination == 0) {
        INFRA_LOG_WARN("[kucoin] [get_position] [fail], msg: not found denomination value for {}", pair);
        return;
    }

    double entry_price = str_to_float(entry_price_text);
    double size = str_to_float(size_text);
    double position_size = size * denomination;

    if (position_size > 0) {
        pos_info->long_size = position_size;
        pos_info->long_open_price = entry_price;
    } else if (position_size < 0) {
        pos_info->short_size = -position_size; // 取正数
        pos_info->short_open_price = entry_price;
    }
}

void parse_classic_position(const simdjson::dom::element& doc, const Symbol& symbol, UMSymbolPosition& res) {
    res.clear();
    if (doc["data"].is_array()) {
        simdjson::dom::array array = doc["data"];
        for (auto obj : array) {
            parse_classic_position_object(obj, symbol, res);
        }
    } else if (doc["data"].is_object()) {
        simdjson::dom::object obj = doc["data"];
        parse_classic_position_object(obj, symbol, res);
    }
}

void parse_unified_position(const simdjson::dom::element& doc, const Symbol& symbol, UMSymbolPosition& res) {
    res.clear();
    simdjson::dom::array array = doc["data"];
    for (auto obj : array) {
        parse_unified_position_object(obj, symbol, res);
    }
}

SpOrder parse_classic_query_order(const simdjson::dom::object& obj) {
    std::string_view symbol = obj["symbol"];
    std::string_view clientOid = obj["clientOid"];
    std::string_view id = obj["id"];
    std::string_view status = obj["status"];
    std::string_view type = obj["type"];
    std::string_view price_text = obj["price"];
    std::int64_t size = obj["size"];
    std::int64_t filled_size = obj["filledSize"];
    std::string_view avg_deal_price = obj["avgDealPrice"];
    bool cancel_exist = obj["cancelExist"];
    std::int64_t createdAt = obj["createdAt"];
    std::int64_t updatedAt = obj["updatedAt"];

    Symbol pair = transfer_to_infra_pair(symbol);
    ClientOrderId client_order_id(clientOid);
    OrderId order_id(id);
    auto rtn_order = std::make_shared<Order>(pair, client_order_id, order_id);

    double denomination = get_denomination_value(pair); // 合约张数转币数
    if (denomination == 0) {
        INFRA_LOG_WARN("[kucoin] [parse_query_order] [fail], msg: not found denomination value for {}", pair);
    }

    OrderStatus order_status = OrderStatus::New;
    if (status == "open") {
        if (filled_size > 0) {
            order_status = OrderStatus::PartiallyFilled;
        } else if (cancel_exist) {
            order_status = OrderStatus::Canceling;
        } else {
            order_status = OrderStatus::New;
        }
    } else if (status == "done") {
        if (cancel_exist) {
            order_status = OrderStatus::Canceled;
        } else {
            order_status = OrderStatus::Filled;
        }
    }

    OrderType order_type = OrderType::Limit;
    if (type == "limit") {
        order_type = OrderType::Limit;
    } else if (type == "market") {
        order_type = OrderType::Market;
    }

    rtn_order->type = order_type;
    rtn_order->status = order_status;
    rtn_order->price = str_to_float(price_text);
    rtn_order->quantity = size * denomination;
    rtn_order->avg_price = str_to_float(avg_deal_price);
    rtn_order->cum_deal_base = filled_size * denomination;

    rtn_order->cum_deal_quote = rtn_order->cum_deal_base * rtn_order->avg_price;
    rtn_order->exchange_create_time = createdAt;
    rtn_order->exchange_update_time = updatedAt;
    return rtn_order;
}

SpOrder parse_unified_query_order(const simdjson::dom::object& obj) {
    std::string_view symbol = obj["symbol"];
    std::string_view clientOid = obj["clientOid"];
    std::string_view orderId = obj["orderId"];
    std::int64_t status = obj["status"];
    std::string_view order_type_text = obj["orderType"];
    std::string_view price_text = obj["price"].get_string_length() == 0 ? std::string_view{"0"} : obj["price"];
    std::string_view size_text = obj["size"];
    std::string_view filled_size = obj["filledSize"];
    std::string_view avg_price = obj["avgPrice"];
    std::int64_t order_time = obj["orderTime"];
    std::int64_t updated_time = obj["updatedTime"];

    Symbol pair = transfer_to_infra_pair(symbol);
    ClientOrderId client_order_id(clientOid);
    OrderId order_id(orderId);
    auto rtn_order = std::make_shared<Order>(pair, client_order_id, order_id);

    double denomination = get_denomination_value(pair); // 合约张数转币数
    if (denomination == 0) {
        INFRA_LOG_WARN("[kucoin] [parse_query_order] [fail], msg: not found denomination value for {}", pair);
    }

    OrderStatus order_status{};
    switch (status) {
        case 0:
        case 1:
        case 2:
            order_status = OrderStatus::New;
            break;
        case 3:
            order_status = OrderStatus::Filled;
            break;
        case 4:
            order_status = OrderStatus::PartiallyFilled;
            break;
        case 5:
        case 6:
            order_status = OrderStatus::Canceled;
            break;
        default:
            order_status = OrderStatus::New;
            break;
    }

    OrderType order_type = OrderType::Limit;
    if (order_type_text == "LIMIT") {
        order_type = OrderType::Limit;
    } else if (order_type_text == "MARKET") {
        order_type = OrderType::Market;
    }

    rtn_order->type = order_type;
    rtn_order->status = order_status;
    rtn_order->price = str_to_float(price_text);
    rtn_order->quantity = str_to_float(size_text) * denomination;
    rtn_order->avg_price = str_to_float(avg_price);
    rtn_order->cum_deal_base = str_to_float(filled_size) * denomination;

    rtn_order->cum_deal_quote = rtn_order->cum_deal_base * rtn_order->avg_price;
    rtn_order->exchange_create_time = order_time / 1'000'000L;
    rtn_order->exchange_update_time = updated_time / 1'000'000L;
    return rtn_order;
}

SpOrder parse_classic_rtn_order(const simdjson::dom::object& obj) {
    std::string_view symbol = obj["symbol"];
    std::string_view clientOid = obj["clientOid"];
    std::string_view orderId = obj["orderId"];
    std::string_view type = obj["type"];
    std::string_view price_text = obj["price"];
    std::string_view size_text = obj["size"];
    std::string_view filledSize = obj["filledSize"];
    std::string_view remainSize = obj["remainSize"];
    std::int64_t order_time = obj["orderTime"];
    std::int64_t ts = obj["ts"];

    Symbol pair = transfer_to_infra_pair(symbol);
    ClientOrderId client_order_id(clientOid);
    OrderId order_id(orderId);
    auto rtn_order = std::make_shared<Order>(pair, client_order_id, order_id);

    double denomination = get_denomination_value(pair); // 合约张数转币数
    if (denomination == 0) {
        INFRA_LOG_WARN("[kucoin] [parse_rtn_order] [fail], msg: not found denomination value for {}", pair);
    }

    OrderStatus order_status = OrderStatus::New;
    if (type == "open" or type == "update")
        order_status = OrderStatus::New;
    else if (type == "filled")
        order_status = OrderStatus::Filled;
    else if (type == "canceled")
        order_status = OrderStatus::Canceled;
    else if (type == "match") {
        if (remainSize != "0") {
            order_status = OrderStatus::PartiallyFilled;
        } else {
            order_status = OrderStatus::Filled;
        }
        std::string_view matchPrice = obj["matchPrice"];
        rtn_order->avg_price = str_to_float(matchPrice);
        rtn_order->cum_deal_base = str_to_float(filledSize) * denomination;
        rtn_order->cum_deal_quote = rtn_order->cum_deal_base * rtn_order->avg_price;
    }

    rtn_order->status = order_status;
    rtn_order->price = str_to_float(price_text);
    rtn_order->quantity = str_to_float(size_text) * denomination;
    rtn_order->exchange_create_time = order_time;
    rtn_order->exchange_update_time = ts;
    return rtn_order;
}

SpOrder parse_unified_rtn_order(const simdjson::dom::object& obj) {
    std::string_view symbol = obj["s"];
    std::string_view clientOid = obj["ci"];
    std::string_view orderId = obj["oi"];
    std::string_view price_text = obj["p"].get_string_length() == 0 ? std::string_view{"0"} : obj["p"];
    std::string_view size_text = obj["q"];
    std::string_view filledSize = obj["fS"];
    std::string_view avgPrice = obj["aP"];
    std::int64_t status = obj["os"];
    std::int64_t create_time = obj["O"];
    std::int64_t update_time = obj["U"];

    Symbol pair = transfer_to_infra_pair(symbol);
    ClientOrderId client_order_id(clientOid);
    OrderId order_id(orderId);
    auto rtn_order = std::make_shared<Order>(pair, client_order_id, order_id);

    double denomination = get_denomination_value(pair); // 合约张数转币数
    if (denomination == 0) {
        INFRA_LOG_WARN("[kucoin] [parse_rtn_order] [fail], msg: not found denomination value for {}", pair);
    }

    OrderStatus order_status{};
    switch (status) {
        case 0:
        case 1:
        case 2:
            order_status = OrderStatus::New;
            break;
        case 3:
            order_status = OrderStatus::Filled;
            break;
        case 4:
            order_status = OrderStatus::PartiallyFilled;
            break;
        case 5:
        case 6:
            order_status = OrderStatus::Canceled;
            break;
        default:
            order_status = OrderStatus::New;
            break;
    }

    rtn_order->avg_price = str_to_float(avgPrice);
    rtn_order->cum_deal_base = str_to_float(filledSize) * denomination;
    rtn_order->cum_deal_quote = rtn_order->cum_deal_base * rtn_order->avg_price;

    rtn_order->status = order_status;
    rtn_order->price = str_to_float(price_text);
    rtn_order->quantity = str_to_float(size_text) * denomination;
    rtn_order->exchange_create_time = create_time;
    rtn_order->exchange_update_time = update_time;
    return rtn_order;
}

SpFundingFee parse_classic_funding_fee(const simdjson::dom::element& doc, const Symbol& symbol) {
    auto data = doc["data"];
    double fundingRate = data["value"];
    std::int64_t fundingTime = data["fundingTime"];

    Timestamp next_milli = fundingTime;
    return std::make_shared<FundingFee>(symbol, time_get_now_milli(), fundingRate, next_milli, 0);
}

void parse_classic_pairs_info(const simdjson::dom::element& doc, const Currency& currency) {
    g_pairs_info_cache.clear();
    g_all_symbols.clear();
    simdjson::dom::array array = doc["data"];
    for (auto item : array) {
        std::string_view symbol_text = item["symbol"];

        // NOTE：过滤不包含USDT的合约
        if (symbol_text.find("USDTM") == std::string_view::npos)
            continue;

        // NOTE：转成统一格式 base-quote
        std::string pair = transfer_to_infra_pair(symbol_text);

        // 检查交易状态
        std::string_view status;
        if (item["status"].get(status) == simdjson::SUCCESS && status != "Open") {
            continue;
        }

        // --- 提取过滤器参数 ---
        double tick_size = item["tickSize"].get_double();
        double multiplier = item["multiplier"].get_double();

        SpExPairInfo pair_info = std::make_shared<ExchangePairInfo>();
        pair_info->pair = pair;
        pair_info->trading_min_base = multiplier;
        pair_info->step_size_base = multiplier;
        pair_info->step_size_quote = tick_size;
        pair_info->denomination_value = multiplier;

        g_pairs_info_cache[pair] = pair_info;
        g_all_symbols.push_back(pair);
    }
}

SpFundingFee parse_unified_funding_fee(const simdjson::dom::element& doc, const Symbol& symbol) {
    auto data = doc["data"];
    double fundingRate = data["nextFundingRate"];
    std::int64_t fundingTime = data["fundingTime"];

    Timestamp next_milli = fundingTime;
    return std::make_shared<FundingFee>(symbol, time_get_now_milli(), fundingRate, next_milli, 0);
}

void parse_unified_pairs_info(const simdjson::dom::element& doc, const Currency& currency) {
    g_pairs_info_cache.clear();
    g_all_symbols.clear();
    simdjson::dom::array array = doc["data"]["list"];
    for (auto item : array) {
        std::string_view symbol_text = item["symbol"];

        // NOTE：过滤不包含USDT的合约
        if (symbol_text.find("USDTM") == std::string_view::npos)
            continue;

        // NOTE：转成统一格式 base-quote
        std::string pair = transfer_to_infra_pair(symbol_text);

        // 检查交易状态
        std::string_view status;
        if (item["tradingStatus"].get(status) == simdjson::SUCCESS && status != "1") {
            continue;
        }

        // --- 提取过滤器参数 ---
        std::string_view tickSize = item["tickSize"];
        std::string_view unitSize = item["unitSize"];

        SpExPairInfo pair_info = std::make_shared<ExchangePairInfo>();
        pair_info->pair = pair;
        pair_info->trading_min_base = str_to_float(unitSize);
        pair_info->step_size_base = str_to_float(unitSize);
        pair_info->step_size_quote = str_to_float(tickSize);
        pair_info->denomination_value = str_to_float(unitSize);

        g_pairs_info_cache[pair] = pair_info;
        g_all_symbols.push_back(pair);
    }
}

std::string get_ws_url(const AccountSecret& secret) {
    std::string now = std::to_string(time_get_now_milli());
    std::string msg = secret.api_key + now;
    std::string signed_data = generate_sign_hmac256_b64(secret.api_secret, msg);
    std::string signed_passphare = generate_sign_hmac256_b64(secret.api_secret, secret.api_phrase);
    std::string trade_params = fmt::format("/v1/private?apikey={}&timestamp={}&sign={}&passphrase={}", secret.api_key,
                                           now, url_encode(signed_data), url_encode(signed_passphare));
    return trade_params;
}
} // namespace infra::kucoin
