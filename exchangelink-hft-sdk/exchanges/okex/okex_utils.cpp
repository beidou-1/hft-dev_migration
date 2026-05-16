#include "okex_utils.h"

namespace infra::okex {

void parse_balance(const Currency& currency, const std::string& raw_str, UMCurrencyBalance& res) {
    // INFRA_LOG_DEBUG("[okex] [parse_balance], content: {}", raw_str);
    try {
        PARSE_JSON(raw_str, doc);
        simdjson::dom::array data = doc["data"];
        for (auto item : data) {
            simdjson::dom::array asset_array = item["details"];
            for (auto asset_item : asset_array) {
                std::string_view currency_text = asset_item["ccy"];
                std::string asset(currency_text);
                // NOTE：currency值为空时返回所有，不为空时只返回currency对应的资产
                if (!currency.empty() && !compare_currency(asset, currency)) {
                    continue;
                }

                std::string_view available_text = item["availEq"];
                std::string_view frozen_text = item["imr"];
                double available = str_to_float(available_text);
                double frozen = str_to_float(frozen_text);
                auto account_asset = std::make_shared<Balance>(asset, available, frozen);
                account_asset->withdraw = available;
                res[account_asset->currency] = account_asset;
            }
        }
    } catch (const std::exception& ex) {
        INFRA_LOG_WARN("[okex] [parse_balance] [exception], parse error: {}, content: {}", ex.what(), raw_str);
    }
}

void parse_position(const std::string& raw_str, UMSymbolPosition& res) {
    // INFRA_LOG_DEBUG("[okex] [parse_position], content: {}", raw_str);
    try {
        PARSE_JSON(raw_str, doc);
        if (doc["code"].error() == simdjson::SUCCESS) {
            std::string_view code = doc["code"];
            if (code != "0") {
                INFRA_LOG_WARN("[okex] [get_position] [fail], response: {}", raw_str);
                return;
            }
        }

        simdjson::dom::array position_array = doc["data"];
        for (auto item : position_array) {
            std::string_view symbol_text = item["instId"];
            std::string_view entryPrice_text = item["avgPx"];
            std::string_view positionAmt_text = item["pos"];
            std::string_view positionSide_text = item["posSide"];
            std::string_view liquidationPrice_text = item["liqPx"];
            std::string_view marginType_text = item["mgnMode"];
            std::string_view leverage_text = item["lever"];
            if (positionAmt_text == "0" || positionAmt_text.empty() || entryPrice_text.empty()) {
                continue;
            }
            Symbol pair = transfer_to_infra_pair(symbol_text);
            SpPosition pos_info{nullptr};
            auto it = res.find(pair);
            if (it == res.end()) {
                pos_info = std::make_shared<Position>();
                pos_info->position_mode =
                    (positionSide_text == "net") ? PositionMode::one_way_mode : PositionMode::hedge_mode;
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

            double entry_price = str_to_float(entryPrice_text);
            double position_size = str_to_float(positionAmt_text);
            double denomination = get_denomination_value(pair);
            if (denomination == 0) {
                INFRA_LOG_WARN("[okex] [get_position] [fail], msg: not found denomination value for {}", pair);
            }
            double position_amount = position_size * denomination;

            if (positionSide_text == "long") {
                pos_info->long_size = position_amount;
                pos_info->long_open_price = entry_price;
            } else if (positionSide_text == "short") {
                pos_info->short_size = position_amount;
                pos_info->short_open_price = entry_price;
            } else if (positionSide_text == "net") {
                if (position_amount > 0) {
                    pos_info->long_size = position_amount;
                    pos_info->long_open_price = entry_price;
                } else if (position_amount < 0) {
                    pos_info->short_size = -position_amount; // 取正数
                    pos_info->short_open_price = entry_price;
                }
            }
        }
    } catch (const std::exception& ex) {
        INFRA_LOG_WARN("[okex] [get_position] [exception], parse error: {}, content: {}", ex.what(), raw_str);
    }
}

double parse_margin_ratio(const simdjson::dom::element& doc) {
    simdjson::dom::array data = doc["data"];
    for (auto item : data) {
        std::string_view mgn_ratio = item["mgnRatio"];
        double ratio = str_to_float(mgn_ratio);
        return ratio;
    }
    return 999.0;
}

SpOrder parse_rtn_order(const simdjson::dom::object& obj) {
    std::string_view symbol = obj["instId"];
    std::string_view client_order_id = obj["clOrdId"];
    std::string_view order_id = obj["ordId"];
    std::string_view price = obj["px"];
    std::string_view size = obj["sz"];
    std::string_view deal_size = obj["accFillSz"];
    std::string_view deal_avg_price = obj["avgPx"];
    std::string_view order_status_text = obj["state"];
    std::string_view create_milli_text = obj["cTime"];
    std::string_view update_milli_text = obj["uTime"];
    uint64_t create_milli = std::stoll(std::string(create_milli_text));
    uint64_t update_milli = std::stoll(std::string(update_milli_text));

    Symbol pair = transfer_to_infra_pair(symbol);
    ClientOrderId client_oid(client_order_id);
    OrderId market_oid(order_id);
    auto rtn_order = std::make_shared<Order>(pair, client_oid, market_oid);

    double denomination = get_denomination_value(pair); // 合约张数转币数
    if (denomination == 0) {
        INFRA_LOG_WARN("[okex] [parse_rtn_order] [fail], msg: not found denomination value for {}", pair);
    }

    double filled_qty = str_to_float(deal_size);
    std::string status_text(order_status_text);
    if (status_text == "mmp_canceled") {
        status_text = "canceled";
    }
    std::transform(status_text.begin(), status_text.end(), status_text.begin(), ::toupper);
    OrderStatus order_status = to_order_status(status_text);

    rtn_order->status = order_status;
    rtn_order->price = str_to_float(price);
    rtn_order->quantity = str_to_float(size) * denomination;
    rtn_order->avg_price = str_to_float(deal_avg_price);
    rtn_order->cum_deal_base = filled_qty * denomination;
    rtn_order->cum_deal_quote = rtn_order->cum_deal_base * rtn_order->avg_price;
    rtn_order->exchange_create_time = create_milli;
    rtn_order->exchange_update_time = update_milli;
    return rtn_order;
}

void parse_pairs_info(const std::string& raw_str, const Currency& currency) {
    g_pairs_info_cache.clear();
    g_all_symbols.clear();
    try {
        PARSE_JSON(raw_str, doc);
        simdjson::dom::array symbols_array = doc["data"];
        for (auto symbol_item : symbols_array) {
            std::string_view state = symbol_item["state"];
            std::string_view symbol_text = symbol_item["instId"];
            std::string_view minQty_text = symbol_item["minSz"];
            std::string_view qtyStepSize_text = symbol_item["lotSz"];
            std::string_view tickSz = symbol_item["tickSz"];
            std::string_view ctVal = symbol_item["ctVal"];
            std::string_view settleCcy = symbol_item["settleCcy"];
            Currency quote(settleCcy);
            if (state != "live" || !compare_currency(quote, currency))
                continue;

            uint64_t instIdCode = symbol_item["instIdCode"].get_uint64();
            double denomination_value = str_to_float(ctVal);

            std::string pair = transfer_to_infra_pair(symbol_text);
            double trading_min_base = str_to_float(minQty_text);
            double step_size_base = str_to_float(qtyStepSize_text);

            SpExPairInfo pair_info = std::make_shared<ExchangePairInfo>();
            pair_info->pair = pair;
            pair_info->trading_min_base = trading_min_base * denomination_value;
            pair_info->step_size_base = step_size_base * denomination_value;
            pair_info->step_size_quote = str_to_float(tickSz);
            pair_info->denomination_value = denomination_value;
            pair_info->alias = std::to_string(instIdCode);
            g_pairs_info_cache[pair] = pair_info;
            g_all_symbols.push_back(std::move(pair));
        }
    } catch (const std::exception& ex) {
        INFRA_LOG_WARN("[okex] [parse_pairs_info] [exception], msg: {}, content: {}", ex.what(), raw_str);
    }
    return;
}

SpFundingFee parse_funding_fee(const std::string& raw_str) {
    try {
        PARSE_JSON(raw_str, doc);
        simdjson::dom::array symbols_array = doc["data"];
        if (symbols_array.size() == 0) {
            return nullptr;
        }
        simdjson::dom::object obj = *(symbols_array.begin());
        std::string_view pair = obj["instId"];
        std::string_view fee_text = obj["fundingRate"];
        std::string_view update_time = obj["nextFundingTime"];
        double fee = str_to_float(fee_text);
        Timestamp milli = time_get_now_milli();
        Timestamp next_milli = std::stoll(std::string(update_time));
        return std::make_shared<FundingFee>(transfer_to_infra_pair(pair), milli, fee, next_milli, 0);
    } catch (const std::exception& ex) {
        INFRA_LOG_WARN("[okex] [parse_funding_fee] [exception], msg: {}, content: {}", ex.what(), raw_str);
        return nullptr;
    }
}
} // namespace infra::okex