#include "hbg_utils.h"

namespace infra::hbg {
Errno extract_error_code(std::string_view sv) {
    if (sv.find("timeout") != std::string_view::npos) {
        return Errno::RequestTimeout;
    } else if (sv.find("API-key") != std::string_view::npos) {
        return Errno::AuthFailed;
    } else if (sv.find("insufficient") != std::string_view::npos) {
        return Errno::InsufficientBalance;
    } else if (sv.find("lower than the minimum price") != std::string_view::npos) {
        return Errno::SmallSizeOrder;
    } else if (sv.find("smaller than the value of one contract") != std::string_view::npos) {
        return Errno::SmallSizeOrder;
    } else {
        return Errno::UnknownError;
    }
}

HttpRequestBody get_request_body_with_sign(boost::beast::http::verb method, const std::string& host,
                                           const std::string& path, const std::string& query, const std::string& body,
                                           const AccountSecret& secret) {
    std::map<std::string, std::string> params;
    params["AccessKeyId"] = secret.api_key;
    params["SignatureMethod"] = "HmacSHA256";
    params["SignatureVersion"] = "2";
    params["Timestamp"] = time_get_now_str();

    if (!query.empty()) {
        std::vector<std::string> pairs;
        boost::split(pairs, query, boost::is_any_of("&"));
        for (const auto& pair : pairs) {
            size_t pos = pair.find('=');
            if (pos != std::string::npos) {
                std::string k = pair.substr(0, pos);
                std::string v = pair.substr(pos + 1);
                params[k] = url_encode(v);
            }
        }
    }

    std::string signed_query;
    bool first = true;
    for (const auto& p : params) {
        if (!first)
            signed_query += "&";
        signed_query += p.first + "=" + url_encode(p.second);
        first = false;
    }

    using namespace boost::beast;
    std::string method_str = (method == http::verb::post) ? "POST" : "GET";
    std::string pre_signed = method_str + "\n" + g_base_host + "\n" + path + "\n" + signed_query;
    std::string signature = generate_sign_hmac256_b64(secret.api_secret, pre_signed);
    std::string final_query = signed_query + "&Signature=" + url_encode(signature);

    HttpRequestBody req{method, path, 11};
    req.set(http::field::host, host);
    req.set(http::field::user_agent, BOOST_BEAST_VERSION_STRING);

    req.set(http::field::accept, "*/*");
    req.target(path + (final_query.empty() ? "" : "?" + final_query));
    if (method == http::verb::post) {
        if (!body.empty()) {
            req.body() = body;
            req.set(http::field::content_type, "application/json");
        }
    }

    req.prepare_payload();
    return req;
}

std::string get_websocket_sign(const std::string& host, const std::string& path, const std::string& query,
                               const std::string& op, const std::string& time_str, const AccountSecret& secret) {
    std::map<std::string, std::string> params;
    params["AccessKeyId"] = secret.api_key;
    params["SignatureMethod"] = "HmacSHA256";
    params["SignatureVersion"] = "2";
    params["Timestamp"] = time_str;

    // 3. 解析用户传入的 query 参数（如果有）
    if (!query.empty()) {
        std::vector<std::string> pairs;
        boost::split(pairs, query, boost::is_any_of("&"));
        for (const auto& pair : pairs) {
            size_t pos = pair.find('=');
            if (pos != std::string::npos) {
                std::string k = pair.substr(0, pos);
                std::string v = pair.substr(pos + 1);
                params[k] = url_encode(v);
            }
        }
    }

    std::string signed_query;
    bool first = true;
    for (const auto& p : params) {
        if (!first)
            signed_query += "&";
        signed_query += p.first + "=" + url_encode(p.second);
        first = false;
    }

    std::string pre_signed = "GET\n" + g_base_host + "\n" + path + "\n" + signed_query;
    std::string signature = generate_sign_hmac256_b64(secret.api_secret, pre_signed);
    return signature;
}

void parse_balance(const simdjson::dom::element& doc, const Currency& currency, UMCurrencyBalance& res) {
    res.clear();
    simdjson::dom::array array = doc["data"]["details"];
    for (auto account_item : array) {
        std::string_view coin_text = account_item["currency"];
        std::string asset = to_lower_str(std::string(coin_text));

        if (!currency.empty() && !compare_currency(asset, currency)) {
            continue;
        }

        std::string_view cross_available = account_item["available"];
        double available_bf = str_to_float(cross_available);
        auto account_asset = std::make_shared<Balance>(asset, available_bf, 0);
        res[account_asset->currency] = account_asset;
    }
}

void parse_position(const simdjson::dom::element& doc, UMSymbolPosition& res) {
    res.clear();
    auto array = doc["data"].get_array();
    for (auto item : array) {
        std::string_view symbol_text = item["contract_code"];
        std::string_view entry_price_text = item["open_avg_price"];
        std::string_view volume_text = item["volume"];
        std::string_view position_side = item["position_side"];
        std::string_view direction = item["direction"];
        std::string_view liq_price = item["liquidation_price"];

        std::string pair = transfer_to_infra_pair(symbol_text);
        double entry_price = str_to_float(entry_price_text);
        double position = str_to_float(volume_text) * g_pairs_info_cache[pair]->step_size_base;

        SpPosition pos_info{nullptr};
        auto it = res.find(pair);
        if (it == res.end()) {
            pos_info = std::make_shared<Position>();
            pos_info->position_mode = (position_side == "both") ? PositionMode::one_way_mode : PositionMode::hedge_mode;
            pos_info->symbol = pair;
            pos_info->bankrupt_price = str_to_float(liq_price);
            pos_info->leverage = item["lever_rate"].get_int64().value();
            pos_info->update_time = time_get_now_milli();
            res[pos_info->symbol] = pos_info;
        } else {
            pos_info = it->second;
            pos_info->update_time = time_get_now_milli();
        }

        if (position_side == "long") {
            pos_info->long_size = position;
            pos_info->long_open_price = entry_price;
        } else if (position_side == "short") {
            pos_info->short_size = position;
            pos_info->short_open_price = entry_price;
        } else if (position_side == "both" && direction == "buy") {
            pos_info->long_size = position;
            pos_info->long_open_price = entry_price;
        } else if (position_side == "both" && direction == "sell") {
            pos_info->short_size = position;
            pos_info->short_open_price = entry_price;
        }
    }
}

SpOrder parse_rtn_order(const simdjson::dom::object& obj, const std::string& channel) {
    std::string_view symbol = obj["contract_code"];
    std::string_view orderStatus = obj["state"];
    std::string_view cid_sv = "";
    auto res = obj["client_order_id"].get<std::string_view>();
    if (res.error() == simdjson::SUCCESS) {
        cid_sv = res.value();
    }

    Symbol pair = transfer_to_infra_pair(symbol);
    ClientOrderId client_oid(cid_sv);

    OrderId market_oid;

    auto order_id_elem = obj["order_id"];
    if (auto int_res = order_id_elem.get_int64(); int_res.error() == simdjson::SUCCESS) {
        market_oid = std::to_string(int_res.value());
    } else if (auto str_res = order_id_elem.get_string(); str_res.error() == simdjson::SUCCESS) {
        market_oid = str_res.value();
    }

    auto rtn_order = std::make_shared<Order>(pair, client_oid, market_oid);

    OrderStatus order_status = OrderStatus::New;
    if (orderStatus == "new")
        order_status = OrderStatus::New;
    else if (orderStatus == "partially_filled")
        order_status = OrderStatus::PartiallyFilled;
    else if (orderStatus == "filled")
        order_status = OrderStatus::Filled;
    else if (orderStatus == "partially_canceled")
        order_status = OrderStatus::Canceled;
    else if (orderStatus == "canceled")
        order_status = OrderStatus::Canceled;

    rtn_order->status = order_status;

    std::string_view price = obj["price"];
    std::string_view trade_avg_price = obj["trade_avg_price"];
    rtn_order->price = str_to_float(price);
    rtn_order->avg_price = str_to_float(trade_avg_price);
    std::string created_time(obj["created_time"].get_string().value());
    rtn_order->exchange_create_time = std::stoll(created_time);

    double trade_volume =
        str_to_float(obj["trade_volume"].get_string().value()) * g_pairs_info_cache[pair]->step_size_base;
    rtn_order->cum_deal_base = trade_volume;
    rtn_order->cum_deal_quote = rtn_order->cum_deal_base * rtn_order->avg_price;
    rtn_order->quantity = str_to_float(obj["volume"].get_string().value()) * g_pairs_info_cache[pair]->step_size_base;

    std::string update_time;
    auto update_time_res = obj["updated_time"].get_string();
    if (update_time_res.error() == simdjson::SUCCESS) {
        update_time = update_time_res.value();
    }

    rtn_order->exchange_update_time = std::stoll(update_time);
    return rtn_order;
}

SpFundingFee parse_funding_fee(const simdjson::dom::element& doc, const Symbol& pair) {
    std::string_view funding_rate = doc["data"]["funding_rate"];
    std::string_view funding_time_sv = doc["data"]["funding_time"];
    int64_t next_time = std::stoll(std::string(funding_time_sv));
    double fee = str_to_float(funding_rate);
    return std::make_shared<FundingFee>(pair, time_get_now_milli(), fee, next_time, 0);
}

void parse_pairs_info(const simdjson::dom::element& doc, const Currency& currency) {
    auto array = doc["data"].get_array();
    g_pairs_info_cache.clear();
    g_all_symbols.clear();
    for (auto item : array) {
        std::string_view symbol_text = item["contract_code"];
        std::string pair = transfer_to_infra_pair(symbol_text);
        std::string_view status;
        if (item["contract_status"].get_int64().value() != 1 || item["contract_type"].get_string().value() != "swap") {
            continue;
        }

        double contract_size = item["contract_size"].get_double().value();
        SpExPairInfo pair_info = std::make_shared<ExchangePairInfo>();
        pair_info->pair = pair;
        pair_info->trading_min_base = contract_size;
        pair_info->step_size_base = contract_size;
        pair_info->step_size_quote = item["price_tick"].get_double().value();
        pair_info->denomination_value = contract_size;

        g_pairs_info_cache[pair] = pair_info;
        g_all_symbols.push_back(pair);
    }
}
Currency get_right_currency(const Currency& currency) { return to_lower_str(currency); }
std::string hbg_decompress_gzip(std::string_view input) {
    if (input.size() < 10 || static_cast<unsigned char>(input[0]) != 0x1F ||
        static_cast<unsigned char>(input[1]) != 0x8B || static_cast<unsigned char>(input[2]) != 0x08) {
        return std::string(input);
    }

    try {
        return infra::decompress_gzip(input);
    } catch (...) {
        return std::string(input);
    }
}

double parse_margin_ratio(const simdjson::dom::element& doc) {
    simdjson::dom::array array = doc["data"]["details"].get_array();
    for (auto&& item : array) {
        if ( "USDT" != item["currency"].get_string().value() ){
            continue;
        }

        std::string_view maintenance_margin_sv = item["maintenance_margin"];
        std::string_view equity_sv = item["equity"];

        double maintenance_margin = str_to_float(maintenance_margin_sv);
        double equity = str_to_float(equity_sv);
        if (is_zero(maintenance_margin)) {
            return 999.0;
        } else if (is_zero(equity)) {
            return 0.0;
        }
        double mgnRatio = equity / maintenance_margin;
        return mgnRatio;
    }
    return 999.0;
}

} // namespace infra::hbg