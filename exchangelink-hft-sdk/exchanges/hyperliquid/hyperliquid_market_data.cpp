#include "hyperliquid_market_data.h"
using namespace infra::hyperliquid;

namespace infra {
bool HyperliquidMarketData::initialize() {
    auto& info = g_config_map[base_config_.to_str()];
    if (info.empty()) {
        INFRA_LOG_WARN("[hyperliquid] [initialize] [fail], msg: {} {} {} not implemented",
                       to_string(base_config_.account_type), to_string(base_config_.address_type),
                       to_string(base_config_.settle_unit));
        return false;
    }

    rest_host_ = info[REST_HOST];
    pairs_info_path_ = info[PAIRS_INFO_PATH];
    funding_fee_path_ = info[FUNDING_FEE_PATH];
    wss_infos_ = {info[WSS_PUBLIC_HOST], info[WSS_PORT], info[WSS_PUBLIC_PATH]};
    INFRA_LOG_INFO("[hyperliquid] [initialize] [MarketData], websocket endpoint: {} {} {}", wss_infos_.host,
                   wss_infos_.path, wss_infos_.port);
    return true;
}

void HyperliquidMarketData::shutdown() { unsubscribe_orderbook(); }

bool HyperliquidMarketData::subscribe_orderbook(const Symbols& symbols, unsigned int depth, OrderbookCallback cb) {
    if (depth != 1) {
        INFRA_LOG_WARN("[hyperliquid] [subscribe_orderbook] [fail], msg: unsupported depth level {}, only 1 is supported",
                       depth);
        return false;
    }

    this->orderbook_handler_ = std::move(cb);
    const Symbols& sub_symbols = (!symbols.empty()) ? symbols : g_all_symbols;
    size_t total = sub_symbols.size();
    for (size_t i = 0; i < total; i++) {
        Symbol pair = transfer_from_infra_pair(sub_symbols[i]);
        auto it = g_pairs_info_cache.find(pair);
        if (it == g_pairs_info_cache.end()) {
            INFRA_LOG_WARN("[hyperliquid] [subscribe_orderbook] [fail], msg: not found {} in cache", pair);
            continue;
        }

        size_t pos = pair.find("-usdt");
        if (pos == std::string::npos) {
            INFRA_LOG_WARN("[hyperliquid] [subscribe_orderbook] [fail], msg: unsupported pair: {}", pair);
            continue;
        }
        std::string coin = pair.substr(0, pos); // 去掉后缀
        std::transform(coin.begin(), coin.end(), coin.begin(), ::toupper);
        stream_params_.push_back(std::move(coin));
    }

    size_t connection_nums = (stream_params_.size() / MAX_PAIRS_PER_WS_CONNECTION) + 1;
    INFRA_LOG_INFO("[hyperliquid] [subscribe_orderbook] [success], msg: establishing {} WebSocket connections",
                   connection_nums);
    for (size_t i = 0; i < connection_nums; i++) {
        auto client = std::make_shared<WebSocketClient>(ioc_, ssl_ctx_, *this);
        wss_connections_.push_back(client);
        auto& conn = wss_connections_.back();
        conn->set_user_data(i); // index of wss_connections_
        conn->resolve_connect(wss_infos_.host, wss_infos_.port, wss_infos_.path);
    }
    return true;
}

void HyperliquidMarketData::unsubscribe_orderbook() {
    this->orderbook_handler_ = nullptr;
    for (auto& conn : wss_connections_) {
        if (conn->is_socket_open()) {
            conn->close();
        }
    }
    stream_params_.clear();
    wss_connections_.clear();
    INFRA_LOG_INFO("[hyperliquid] [unsubscribe_orderbook] [success]");
}

void HyperliquidMarketData::fetch_pairs_info(ExPairInfoCallback cb) {
    if (pairs_info_path_.empty()) {
        cb(Errno::NotImplemented, {});
        return;
    }

    std::string query = "{\"type\":\"meta\"}";
    auto req = get_request_body_by_post(rest_host_, pairs_info_path_, query);
    rest_.send(req, [this, cb](HttpResponseBody& res) {
        std::string response = boost::beast::buffers_to_string(res.body().data());
        do {
            if (res.result() != HTTP_STATUS_OK) {
                break;
            }
            try {
                PARSE_JSON(response, doc);
                Currency currency = to_string(base_config_.settle_unit);
                parse_pairs_info(doc, currency);
                INFRA_LOG_INFO("[hyperliquid] [fetch_pairs_info] [success], size: {}", g_pairs_info_cache.size());
                cb(Errno::Ok, g_pairs_info_cache);
                return;
            } catch (const std::exception& ex) {
                INFRA_LOG_WARN("[hyperliquid] [fetch_pairs_info] [exception], msg: {}", ex.what());
            }
        } while (0);
        INFRA_LOG_WARN("[hyperliquid] [fetch_pairs_info] [fail], recv: {}", response);
        cb(extract_error_code(response), {});
    });
}

void HyperliquidMarketData::fetch_funding_fee(const Symbol& symbol, FundingFeeCallback cb) {
    if (funding_fee_path_.empty()) {
        cb(Errno::NotImplemented, nullptr);
        return;
    }

    if (symbol.empty()) {
        cb(Errno::InvalidParams, nullptr);
        return;
    }

    std::string query = "{\"type\":\"metaAndAssetCtxs\"}";
    auto req = get_request_body_by_post(rest_host_, funding_fee_path_, query);
    rest_.send(req, [cb, symbol](HttpResponseBody& res) {
        std::string response = boost::beast::buffers_to_string(res.body().data());
        do {
            if (res.result() != HTTP_STATUS_OK) {
                break;
            }
            try {
                PARSE_JSON(response, doc);
                auto fee = parse_funding_fee(doc, symbol);
                INFRA_LOG_INFO("[hyperliquid] [fetch_funding_fee] [success]");
                cb(Errno::Ok, fee);
                return;
            } catch (const std::exception& ex) {
                INFRA_LOG_WARN("[hyperliquid] [fetch_funding_fee] [exception], msg: {}", ex.what());
            }
        } while (0);
        INFRA_LOG_WARN("[hyperliquid] [fetch_funding_fee] [fail], recv: {}", response);
        cb(extract_error_code(response), nullptr);
    });
}

Action HyperliquidMarketData::on_connect(Wss* ws) {
    size_t index = ws->get_index();
    INFRA_LOG_INFO("[hyperliquid] [on_connect] [MarketData], msg: WebSocket connection established, index: {}", index);
    keep_ws_connection_alive(index);
    subscribe(index);
    return Action::NONE;
}

Action HyperliquidMarketData::on_ping(Wss* ws, std::string_view payload) {
    // INFRA_LOG_DEBUG("[hyperliquid] [on_ping] [MarketData], payload: {}", payload);
    ws->pong(std::string(payload));
    return Action::NONE;
}

Action HyperliquidMarketData::on_pong(Wss* ws, std::string_view payload) {
    // INFRA_LOG_DEBUG("[hyperliquid] [on_pong] [MarketData], payload: {}", payload);
    return Action::NONE;
}

void HyperliquidMarketData::on_close(Wss* ws) {
    size_t index = ws->get_index();
    INFRA_LOG_WARN("[hyperliquid] [on_close] [MarketData], msg: WebSocket connection has been closed, index: {}",
                   index);
}

void HyperliquidMarketData::on_error(Wss* ws, std::string_view err) {
    size_t index = ws->get_index();
    INFRA_LOG_WARN("[hyperliquid] [on_error] [MarketData], msg: WebSocket error occurred: {}, index: {}", err, index);
}

Action HyperliquidMarketData::on_message(Wss* ws, std::string_view msg) {
    // INFRA_LOG_DEBUG("[hyperliquid] [on_message] [MarketData], msg: {}", msg);
    uint64_t recv_tsc = rdtsc();
    uint64_t recv_milli = time_get_now_milli();
    try {
        PARSE_JSON(msg, doc);
        if (doc["channel"].error() == simdjson::SUCCESS) {
            std::string_view channel = doc["channel"];
            if (channel == "bbo") {
                simdjson::dom::object data = doc["data"];
                on_message_bookticker(data, recv_tsc, recv_milli);
            } else if (channel == "subscriptionResponse") {
                INFRA_LOG_INFO("[hyperliquid] [on_message] [subscribe], msg: {}", msg);
            } else if (channel == "pong") {
                // ignore
            } else {
                INFRA_LOG_WARN("[hyperliquid] [on_message] unexpected msg: {}", msg);
            }
        } else {
            INFRA_LOG_WARN("[hyperliquid] [on_message] unexpected msg: {}", msg);
        }
    } catch (const std::exception& ex) {
        INFRA_LOG_WARN("[hyperliquid] [on_message] [exception], error: {}, msg: {}", ex.what(), msg);
    }
    return Action::RECEIVE;
}

void HyperliquidMarketData::keep_ws_connection_alive(size_t index) {
    wss_connections_[index]->start_ping_pong(R"({"method":"ping"})", 30); // 心跳检测时间为60秒
}

void HyperliquidMarketData::subscribe(size_t index) {
    for (size_t i = 0; i < MAX_PAIRS_PER_WS_CONNECTION; i++) {
        size_t id = i + index * MAX_PAIRS_PER_WS_CONNECTION;
        if (id >= this->stream_params_.size()) {
            break;
        }
        std::string payload =
            fmt::format(R"({{"method":"subscribe","subscription":{{"type":"bbo","coin":"{}"}}}})", stream_params_[id]);
        INFRA_LOG_INFO("[hyperliquid] [subscribe_orderbook], connection: {}, send: {}", index, payload);
        wss_connections_[index]->send(std::move(payload));
    }
}

void HyperliquidMarketData::on_message_bookticker(const simdjson::dom::object& data, uint64_t recv_tsc, uint64_t recv_milli) {
    std::string_view coin = data["coin"];
    Timestamp milli = data["time"];
    simdjson::dom::array bbo_array = data["bbo"];

    auto it = bbo_array.begin();
    std::string_view bid0_price_text = (*it)["px"];
    std::string_view bid0_amount_text = (*it)["sz"];
    ++it;
    std::string_view ask0_price_text = (*it)["px"];
    std::string_view ask0_amount_text = (*it)["sz"];

    double best_ask_price = 0.0;
    double best_ask_size = 0.0;
    double best_bid_price = 0.0;
    double best_bid_size = 0.0;
    
    std::list<Level> asks, bids;
    asks.emplace_back(str_to_float(ask0_price_text), str_to_float(ask0_amount_text));
    bids.emplace_back(str_to_float(bid0_price_text), str_to_float(bid0_amount_text));

    Symbol pair = transfer_to_infra_pair(coin);
    SpOrderBook orderbook = this->apply_orderbook_delta( pair, milli, asks, bids, , best_ask_price, best_ask_size, best_bid_price, best_bid_size);
    orderbook->recv_tsc = recv_tsc;
    orderbook->recv_milli = recv_milli;
    orderbook->parsed_tsc = rdtsc();
    this->dispatch_orderbook(std::move(orderbook));
}
} // namespace infra