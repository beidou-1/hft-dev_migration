#include "aster_utils.h"
#include "common/sign_eth.hpp"

namespace infra::aster {
Errno extract_error_code(std::string_view sv) {
    if (sv.find("timeout") != std::string_view::npos) {
        return Errno::RequestTimeout;
    } else if (sv.find("API-key") != std::string_view::npos) {
        return Errno::AuthFailed;
    } else if (sv.find("insufficient") != std::string_view::npos) {
        return Errno::InsufficientBalance;
    } else if (sv.find("notional must be no smaller than") != std::string_view::npos ||
               sv.find("Price less than min price") != std::string_view::npos) {
        return Errno::SmallSizeOrder;
    } else if (sv.find("ReduceOnly Order is rejected") != std::string_view::npos) {
        return Errno::ReduceOnlyRejected;
    } else if (sv.find("Post Only order will be rejected") != std::string_view::npos) {
        return Errno::PostOnlyRejected;
    } else if (sv.find("Unknown order sent") != std::string_view::npos) {
        return Errno::OrderNotFound;
    } else if (sv.find("Order does not exist") != std::string_view::npos) {
        return Errno::OrderNotFound;
    } else if (sv.find("ClientOrderId is duplicated") != std::string_view::npos) {
        return Errno::DuplicatedId;
    } else if (sv.find("ahead of the server's time") != std::string_view::npos) {
        return Errno::TimestampAhead;
    } else if (sv.find("position side does not match") != std::string_view::npos) {
        return Errno::PositionSideWrong;
    } else if (sv.find("Limit price") != std::string_view::npos) {
        return Errno::PercentPrice;
    } else {
        return Errno::UnknownError;
    }
}

HttpRequestBody get_request_body_with_sign(boost::beast::http::verb method, const std::string& host,
                                              const std::string& path, const std::string& query,
                                              const AccountSecret& secret) {
    std::string user = secret.wallet_address;
    std::string signer = secret.custom_info.at("signer");
    std::string nonce_str = std::to_string(time_get_now_micro());

    std::string request_str = query;
    if (!request_str.empty()) {
        request_str.append("&");
    }
    request_str.append("nonce=").append(nonce_str);
    request_str.append("&user=").append(user);
    request_str.append("&signer=").append(signer);

    EcdsaSignature sign = sign_message(request_str, secret);
    request_str.append("&signature=").append(sign.hex);

    using namespace boost::beast;
    std::string url_str = path;
    if (method == http::verb::get || method == http::verb::delete_) {
        url_str.append("?").append(request_str);
    }

    HttpRequestBody req{method, url_str, 11};
    req.set(http::field::host, host);
    req.set(http::field::user_agent, BOOST_BEAST_VERSION_STRING);
    req.set(http::field::connection, "close");
    req.set(http::field::content_type, "application/x-www-form-urlencoded");

    if (method == http::verb::post) {
        req.body() = request_str;
        req.prepare_payload();
    }
    return req;
}

void parse_balance(const simdjson::dom::element& doc, const Currency& currency, UMCurrencyBalance& res) {
    simdjson::dom::array balances_array = doc.get_array();
    for (auto item : balances_array) {
        std::string_view asset_text = item["asset"];
        Currency asset(asset_text);
        if (!currency.empty() && !compare_currency(asset, currency))
            continue;

        std::string_view wallet_balance_text = item["crossWalletBalance"];
        std::string_view unpnl_text = item["crossUnPnl"];
        std::string_view available_text = item["availableBalance"];
        std::string_view withdraw_amount_text = item["maxWithdrawAmount"];
        bfloat equity = str_to_float(wallet_balance_text) + str_to_float(unpnl_text);
        bfloat available = str_to_float(available_text);

        auto account_asset = std::make_shared<Balance>(asset, available, equity - available);
        account_asset->withdraw = str_to_float(withdraw_amount_text);
        res[account_asset->currency] = account_asset;
    }
}

void parse_position(const simdjson::dom::element& doc, UMSymbolPosition& res) {
    simdjson::dom::array position_array = doc.get_array();
    for (auto item : position_array) {
        std::string_view symbol_text = item["symbol"];
        std::string_view entryPrice_text = item["entryPrice"];
        std::string_view positionSide_text = item["positionSide"];
        std::string_view positionAmt_text = item["positionAmt"];
        std::string_view marginType_text = item["marginType"];
        std::string_view liquidationPrice_text = item["liquidationPrice"];
        std::string_view leverage_text = item["leverage"];
        std::string pair = transfer_to_infra_pair(symbol_text);
        bfloat entry_price = str_to_float(entryPrice_text);
        bfloat position_amount = str_to_float(positionAmt_text);

        SpPosition pos_info{nullptr};
        auto it = res.find(pair);
        if (it == res.end()) {
            pos_info = std::make_shared<Position>();
            pos_info->position_mode =
                (positionSide_text == "BOTH") ? PositionMode::one_way_mode : PositionMode::hedge_mode;
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

        if (positionSide_text == "LONG") {
            pos_info->long_size = position_amount;
            pos_info->long_open_price = entry_price;
        } else if (positionSide_text == "SHORT") {
            pos_info->short_size = -position_amount; // 取正数
            pos_info->short_open_price = entry_price;
        } else if (positionSide_text == "BOTH") {
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

SpOrder parse_rtn_order(const simdjson::dom::object& obj, bool intact) {
    int64_t order_id = intact ? obj["orderId"] : obj["i"];
    std::string_view symbol_text = intact ? obj["symbol"] : obj["s"];
    std::string_view client_oid_text = intact ? obj["clientOrderId"] : obj["c"];
    std::string_view order_status_text = intact ? obj["status"] : obj["X"];
    std::string_view price_text = intact ? obj["price"] : obj["p"];
    std::string_view quantity_text = intact ? obj["origQty"] : obj["q"];
    std::string_view accumulated_quantity_text = intact ? obj["executedQty"] : obj["z"];
    std::string_view avg_price_text = intact ? obj["avgPrice"] : obj["ap"];
    int64_t update_milli = intact ? obj["updateTime"] : obj["T"];

    std::string pair = transfer_to_infra_pair(symbol_text);
    std::string client_oid(client_oid_text);
    std::string market_oid = std::to_string(order_id);
    SpOrder rtn_order = std::make_shared<Order>(pair, client_oid, market_oid);

    OrderStatus order_status = to_order_status(order_status_text);
    if (order_status == OrderStatus::Expired) {
        order_status = OrderStatus::Canceled;
    }

    rtn_order->status = order_status;
    rtn_order->quantity = str_to_float(quantity_text);
    rtn_order->price = str_to_float(price_text);
    rtn_order->avg_price = str_to_float(avg_price_text);
    rtn_order->cum_deal_base = str_to_float(accumulated_quantity_text);
    rtn_order->cum_deal_quote = rtn_order->cum_deal_base * rtn_order->avg_price;
    rtn_order->exchange_create_time = update_milli;
    rtn_order->exchange_update_time = update_milli;
    return rtn_order;
}

SpFundingFee parse_funding_fee(const simdjson::dom::element& doc) {
    std::string_view pair = doc["symbol"];
    std::string_view fee_text = doc["lastFundingRate"];
    Timestamp next_milli = doc["nextFundingTime"].get_int64();
    bfloat fee = str_to_float(fee_text);
    Timestamp milli = time_get_now_milli();
    return std::make_shared<FundingFee>(transfer_to_infra_pair(pair), milli, fee, next_milli, 0);
}

void parse_pairs_info(const simdjson::dom::element& doc, const Currency& currency) {
    g_pairs_info_cache.clear();
    g_all_symbols.clear();
    simdjson::dom::array symbols_array = doc["symbols"];
    for (auto symbol_item : symbols_array) {
        std::string_view state = symbol_item["status"];
        std::string_view symbol_text = symbol_item["symbol"];
        std::string_view settleCcy = symbol_item["quoteAsset"];
        Currency quote(settleCcy);
        if (state != "TRADING" || !compare_currency(quote, currency))
            continue;
        bfloat trading_min_base{}, step_size_base{}, step_size_quote{}, min_notional{};
        simdjson::dom::array filters_array = symbol_item["filters"];
        for (auto filter_item : filters_array) {
            std::string_view filter_type = filter_item["filterType"];
            if (filter_type == "LOT_SIZE") {
                std::string_view minQty_text = filter_item["minQty"];
                std::string_view stepSize_text = filter_item["stepSize"];
                trading_min_base = str_to_float(minQty_text);
                step_size_base = str_to_float(stepSize_text);
            }

            if (filter_type == "PRICE_FILTER") {
                std::string_view tickSize_text = filter_item["tickSize"];
                step_size_quote = str_to_float(tickSize_text);
            }

            if (filter_type == "MIN_NOTIONAL") {
                std::string_view minNotional_text = filter_item["notional"];
                min_notional = str_to_float(minNotional_text);
            }
        }
        std::string pair = transfer_to_infra_pair(symbol_text);
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

EcdsaSignature sign_message(const std::string& request_str, const AccountSecret& secret) {
    auto header = eip712_domain_encode("AsterSignTransaction", "1", "1666", "0x0000000000000000000000000000000000000000");

    std::string primary_type = "Message(string msg)";
    std::string type_hash_hex = generate_hash_keccak_hex(primary_type);
    std::string msg_hash_hex = generate_hash_keccak_hex(request_str);
    std::vector<std::string> types = {"bytes32", "bytes32"};
    std::vector<std::string> args = {type_hash_hex, msg_hash_hex};
    auto bodyer = eip712_struct_encode(types, args);

    auto message = eip712_message(header, bodyer);
    return generate_sign_ecdsa(message, secret.api_secret);
}
} // namespace infra::aster