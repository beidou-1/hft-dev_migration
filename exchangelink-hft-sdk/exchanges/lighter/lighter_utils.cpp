#include "lighter_utils.h"
#include <future>

namespace infra::lighter {
// id只能是纯数字
bool check_client_id(const ClientOrderId& oid) {
    return oid.empty() || std::all_of(oid.begin(), oid.end(), [](unsigned char c) {
               return (c >= '0' && c <= '9'); // 数字
           });
}


Errno extract_error_code(std::string_view sv) {
    if (sv.find("timeout") != std::string_view::npos) {
        return Errno::RequestTimeout;
    } else if (sv.find("API-key") != std::string_view::npos) {
        return Errno::AuthFailed;
    } else if (sv.find("insufficient") != std::string_view::npos) {
        return Errno::InsufficientBalance;
    } else if (sv.find("transaction not found") != std::string_view::npos) {
        return Errno::OrderNotFound;
    } else if (sv.find("invalid order base or quote amount") != std::string_view::npos) {
        return Errno::SmallSizeOrder;
    } else {
        return Errno::UnknownError;
    }
}

HttpRequestBody get_request_body_with_tx(const std::string& host, int txType, const std::string& txInfo) {
    std::string path = "/api/v1/sendTx";
    std::string boundary = "----MyBoundary123456";
    std::string body{};

    body += "--" + boundary + "\r\n";
    body += "Content-Disposition: form-data; name=\"tx_type\"\r\n\r\n";
    body += std::to_string(txType) + "\r\n";

    body += "--" + boundary + "\r\n";
    body += "Content-Disposition: form-data; name=\"tx_info\"\r\n\r\n";
    body += txInfo + "\r\n";

    body += "--" + boundary + "--\r\n";

    using namespace boost::beast;
    HttpRequestBody req{http::verb::post, path, 11};
    req.set(http::field::host, host);
    req.set(http::field::user_agent, BOOST_BEAST_VERSION_STRING);
    req.set(http::field::connection, "close");
    req.set(http::field::accept, "application/json");
    req.set(http::field::content_type, "multipart/form-data; boundary=" + boundary);

    req.body() = body;
    req.prepare_payload();
    return req;
}

void parse_balance(const simdjson::dom::element& doc, const Currency& currency, UMCurrencyBalance& res) {
    res.clear();
    simdjson::dom::array account_array = doc["accounts"];
    for (auto account_item : account_array) {
        std::string asset("usdc"); // 仅支持USDC
        if (!currency.empty() && !compare_currency(asset, currency)) {
            continue;
        }

        std::string_view available_balance = account_item["available_balance"];
        std::string_view total_asset_value = account_item["total_asset_value"];
        double available = str_to_float(available_balance);
        double frozen = str_to_float(total_asset_value) - available;

        auto balance_info = std::make_shared<Balance>(asset, available, frozen);
        balance_info->withdraw = balance_info->available;
        res[balance_info->currency] = balance_info;
    }
}

void parse_position(const simdjson::dom::element& doc, UMSymbolPosition& res) {
    res.clear();
    simdjson::dom::array account_array = doc["accounts"];
    for (auto account_item : account_array) {
        simdjson::dom::array array = account_item["positions"];
        for (auto item : array) {
            std::string_view symbol = item["symbol"];
            std::string_view position = item["position"];
            std::string_view avg_entry_price = item["avg_entry_price"];
            std::string_view liquidation_price = item["liquidation_price"];
            int64_t margin_mode = item["margin_mode"];
            int64_t sign = item["sign"];

            double entry_price = str_to_float(avg_entry_price);
            double position_amount = str_to_float(position);
            Symbol pair = transfer_to_infra_pair(symbol);
            SpPosition pos_info{nullptr};
            auto it = res.find(pair);
            if (it == res.end()) {
                pos_info = std::make_shared<Position>();
                pos_info->position_mode = PositionMode::one_way_mode; // 仅支持单向持仓
                pos_info->margin_mode = (margin_mode == 0) ? MarginMode::CROSS : MarginMode::ISOLATED;
                pos_info->symbol = pair;
                pos_info->bankrupt_price = str_to_float(liquidation_price);
                pos_info->update_time = time_get_now_milli();
                res[pos_info->symbol] = pos_info;
            } else {
                pos_info = it->second;
                pos_info->update_time = time_get_now_milli();
            }

            if (sign == 1) {
                pos_info->long_size = position_amount;
                pos_info->long_open_price = entry_price;
            } else if (sign == -1) {
                pos_info->short_size = position_amount;
                pos_info->short_open_price = entry_price;
            }
        }
    }
}

SpOrder parse_rtn_order(const simdjson::dom::object& obj) {
    std::string_view client_order_id = obj["client_order_id"];
    int64_t market_index = obj["market_index"];
    std::string_view status = obj["status"];
    std::string_view price = obj["price"];
    std::string_view initial_base_amount = obj["initial_base_amount"];
    std::string_view filled_base_amount = obj["filled_base_amount"];
    std::string_view filled_quote_amount = obj["filled_quote_amount"];
    Timestamp created_at = obj["created_at"]; // 秒数
    Timestamp updated_at = obj["updated_at"]; // 秒数

    Symbol pair = g_market_id_to_symbol[market_index];
    ClientOrderId client_oid(client_order_id);
    auto rtn_order = std::make_shared<Order>(pair, client_oid, "");

    OrderStatus order_status = OrderStatus::Created;
    if (status == "open") {
        order_status = OrderStatus::New;
    } else if (status == "filled") {
        order_status = OrderStatus::Filled;
    } else if (status == "canceled") {
        order_status = OrderStatus::Canceled;
    } else if (status == "canceled-too-much-slippage" || status == "canceled-reduce-only" ||
               status == "canceled-not-enough-liquidity") {
        order_status = OrderStatus::Rejected;
        rtn_order->detail = status;
        if (status == "canceled-reduce-only") {
            rtn_order->ec = Errno::ReduceOnlyRejected;
        }
        if (status == "canceled-not-enough-liquidity") {
            rtn_order->ec = Errno::IocRejected;
        }
    }

    rtn_order->status = order_status;
    rtn_order->price = str_to_float(price);
    rtn_order->quantity = str_to_float(initial_base_amount);
    rtn_order->cum_deal_base = str_to_float(filled_base_amount);
    rtn_order->cum_deal_quote = str_to_float(filled_quote_amount);
    if (rtn_order->cum_deal_base > 0) {
        rtn_order->avg_price = rtn_order->cum_deal_quote / rtn_order->cum_deal_base;
    }
    rtn_order->exchange_create_time = created_at * 1000;
    rtn_order->exchange_update_time = updated_at * 1000;
    return rtn_order;
}

SpOrder parse_tx_order(const simdjson::dom::object& obj) {
    std::string_view hash = obj["hash"];
    std::string_view info = obj["info"];
    std::string_view event_info = obj["event_info"];
    int64_t type = obj["type"];
    int64_t status = obj["status"];

    auto rtn_order = std::make_shared<Order>();

    OrderStatus order_status = OrderStatus::Created;
    if (type == 14) { // TxTypeL2CreateOrder
        order_status = (status == 2 || status == 3) ? OrderStatus::Filled : OrderStatus::New;
    } else if (type == 15) { // TxTypeL2CancelOrder
        order_status = (status == 2 || status == 3) ? OrderStatus::Canceled : OrderStatus::Canceling;
    }

    if (status == 0) {
        order_status = OrderStatus::Failed;
    }

    try {
        PARSE_JSON(info, info_doc);
        // ClientOrderIndex = info_doc["ClientOrderIndex"];
        int64_t MarketIndex = info_doc["MarketIndex"];
        Symbol pair = g_market_id_to_symbol[MarketIndex];
        SpExPairInfo pair_info = get_pair_info(pair);
        if (pair_info == nullptr) {
            INFRA_LOG_WARN("[lighter] [parse_tx_order] [pair_info is null], pair: {}", pair);
            return nullptr;
        }

        int64_t BaseAmount = info_doc["BaseAmount"];
        int64_t Price = info_doc["Price"];
        rtn_order->market_oid = hash; // OrderId
        rtn_order->price = Price * pair_info->step_size_quote;
        rtn_order->quantity = BaseAmount * pair_info->step_size_base;

        {
            PARSE_JSON(event_info, event_info_doc);
            simdjson::dom::object tobj = event_info_doc["t"];
            int64_t size = tobj["s"];
            int64_t price = tobj["p"];
            if (size == 0) {
                order_status = OrderStatus::New;
            }
            rtn_order->avg_price = price * pair_info->step_size_quote;
            rtn_order->cum_deal_base = size * pair_info->step_size_base;
            rtn_order->cum_deal_quote = rtn_order->cum_deal_base * rtn_order->avg_price;
        }
    } catch (const std::exception& ex) {
        INFRA_LOG_WARN("[lighter] [parse_tx_order] [exception], error: {}, info: {}, event_info: {}", ex.what(), info,
                       event_info);
        return nullptr;
    }

    rtn_order->status = order_status;
    return rtn_order;
}

SpFundingFee parse_funding_fee(const simdjson::dom::element& doc, const Symbol& symbol) {
    simdjson::dom::array array = doc["funding_rates"];
    for (auto item : array) {
        std::string_view exchange = item["exchange"];
        std::string_view symbol_text = item["symbol"];
        Symbol pair = transfer_to_infra_pair(symbol_text);
        if (exchange == "lighter" && compare_currency(pair, symbol)) {
            double fee = item["rate"];
            return std::make_shared<FundingFee>(pair, time_get_now_milli(), fee);
        }
    }
    return nullptr;
}

void parse_pairs_info(const simdjson::dom::element& doc) {
    g_market_id_to_symbol.clear();
    g_pairs_info_cache.clear();
    g_all_symbols.clear();
    simdjson::dom::array array = doc["order_books"];
    for (auto item : array) {
        std::string_view symbol = item["symbol"];
        std::string_view status = item["status"];
        int64_t market_id = item["market_id"];
        std::string_view market_type = item["market_type"];
        std::string_view min_base_amount = item["min_base_amount"];
        std::string_view min_quote_amount = item["min_quote_amount"];
        int64_t supported_size_decimals = item["supported_size_decimals"];
        int64_t supported_price_decimals = item["supported_price_decimals"];

        // NOTE：过滤不活跃的合约
        if (status != "active" || market_type != "perp") {
            continue;
        }

        Symbol pair = transfer_to_infra_pair(symbol);
        auto pair_info = std::make_shared<ExchangePairInfo>();
        pair_info->pair = pair;
        pair_info->trading_min_base = str_to_float(min_base_amount);
        pair_info->step_size_base = get_step_by_decimals(supported_size_decimals);
        pair_info->step_size_quote = get_step_by_decimals(supported_price_decimals);
        pair_info->min_size_quote = str_to_float(min_quote_amount);
        pair_info->alias = std::to_string(market_id);

        g_market_id_to_symbol[market_id] = pair;
        g_pairs_info_cache[pair] = pair_info;
        g_all_symbols.push_back(std::move(pair));
    }
}

double parse_margin_ratio(const simdjson::dom::element& doc) {
    simdjson::dom::array account_array = doc["accounts"];
    for (auto account_item : account_array) {
        double total_asset_value = str_to_float(account_item["total_asset_value"]);
        double available_balance = str_to_float(account_item["available_balance"]);
        double margin_used = total_asset_value - available_balance;
        if (margin_used <= 0.0) {
            return 999.0;
        }
        return total_asset_value / margin_used;
    }
    return 999.0;
}

bool init_lighter_signer(AccountSecret& sec) {
    if (sec.api_secret.empty()) {
        INFRA_LOG_WARN("[lighter] [initialize] [fail], msg: AccountSecret filed: api_secret is empty");
        return false;
    }

    if (sec.custom_info.count("api_key_index") == 0) {
        INFRA_LOG_WARN("[lighter] [initialize] [fail], msg: AccountSecret filed: custom_info[api_key_index] is empty");
        return false;
    }

    if (sec.custom_info.count("api_token") == 0) {
        INFRA_LOG_WARN("[lighter] [initialize] [fail], msg: AccountSecret filed: custom_info[api_token] is empty");
        return false;
    }

    if (sec.custom_info.count("account_id") == 0) {
        INFRA_LOG_WARN("[lighter] [initialize] [fail], msg: AccountSecret filed: custom_info[account_id] is empty");
        return false;
    }

    g_api_token = sec.custom_info["api_token"];
    g_key_index = std::stoi(sec.custom_info["api_key_index"]);
    g_account_index = std::stoll(sec.custom_info["account_id"]);

    std::string url = "https://mainnet.zklighter.elliot.ai";
    char* err = CreateClient(url.data(), sec.api_secret.data(), 304, g_key_index, g_account_index);
    if (err != nullptr) {
        INFRA_LOG_WARN("[lighter] [init_lighter_signer] CreateClient err: {}", err);
        return false;
    }

    char* check = CheckClient(g_key_index, g_account_index);
    if (check != nullptr) {
        INFRA_LOG_WARN("[lighter] [init_lighter_signer] CheckClient msg: {}", check);
        return false;
    }
    return true;
}

void NonceManager::update() { ++nonce_; }

long long int NonceManager::get(HttpClient& client) { return (nonce_ == 0) ? peek(client) : nonce_; }

long long int NonceManager::peek(HttpClient& client) {
    static std::string host = "mainnet.zklighter.elliot.ai";
    static std::string path = "/api/v1/nextNonce";
    std::string query{};
    query.append("account_index=").append(std::to_string(g_account_index));
    query.append("&api_key_index=").append(std::to_string(g_key_index));
    auto req = get_request_body(host, path, query);

    std::string msg;
    {
        net::io_context ioc;
        ssl::context ctx{ssl::context::sslv23_client};
        HttpClient tmp(ioc, ctx);
        std::promise<std::string> p;
        auto f = p.get_future();
        tmp.send(std::move(req), [&p](HttpResponseBody& res) {
            p.set_value(boost::beast::buffers_to_string(res.body().data()));
        });
        ioc.run();
        msg = f.get();
    }

    do {
        try {
            PARSE_JSON(msg, doc);
            if (doc["code"].get_int64() != LIGHTER_SUCCESS_CODE) {
                break;
            }
            nonce_ = doc["nonce"].get_int64();
            INFRA_LOG_INFO("[lighter] [nextNonce] [success], recv: {}", msg);
            return nonce_;
        } catch (const std::exception& ex) {
            INFRA_LOG_WARN("[lighter] [nextNonce] [exception], msg: {}", ex.what());
        }
    } while (0);
    INFRA_LOG_WARN("[lighter] [nextNonce] [fail], recv: {}", msg);
    return 0;
}
} // namespace infra::lighter