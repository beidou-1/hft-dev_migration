#include "aster_market_data.h"
using namespace infra::aster;

namespace infra {
bool AsterMarketData::initialize() {
    auto& info = g_config_map[base_config_.to_str()];
    if (info.empty()) {
        INFRA_LOG_WARN("[aster] [initialize] [fail], msg: {} {} {} not implemented",
                       to_string(base_config_.account_type), to_string(base_config_.address_type),
                       to_string(base_config_.settle_unit));
        return false;
    }

    rest_host_ = info[REST_HOST];
    pairs_info_path_ = info[PAIRS_INFO_PATH];
    funding_fee_path_ = info[FUNDING_FEE_PATH];
    wss_infos_ = {info[WSS_PUBLIC_HOST], info[WSS_PORT], info[WSS_PUBLIC_PATH]};
    INFRA_LOG_INFO("[aster] [initialize] [MarketData], websocket endpoint: {} {} {}", wss_infos_.host, wss_infos_.path,
                   wss_infos_.port);
    return true;
}

void AsterMarketData::shutdown() { unsubscribe_orderbook(); }

bool AsterMarketData::subscribe_orderbook(const Symbols& symbols, unsigned int depth, OrderbookCallback cb) {
    if (depth != 1 && depth != 5 && depth != 10 && depth != 20) {
        INFRA_LOG_WARN("[aster] [subscribe_orderbook] [fail], msg: unsupported depth level {}", depth);
        return false;
    }

    std::string sub_event{};
    if (depth == 1) {
        sub_event = "bookTicker";
    } else {
        sub_event = "depth" + std::to_string(depth) + "@100ms";
    }
    orderbook_handler_ = std::move(cb);
    const Symbols& sub_symbols = (!symbols.empty()) ? symbols : g_all_symbols;
    size_t total = sub_symbols.size();
    std::ostringstream oss{};
    for (size_t i = 1; i <= total; i++) {
        std::string pair = transfer_from_infra_pair(sub_symbols[i - 1]);
        std::transform(pair.begin(), pair.end(), pair.begin(), ::tolower);
        oss << "\"" << pair << "@" << sub_event << "\",";
        if (i % MAX_PAIRS_PER_WS_CONNECTION == 0 || i == total) {
            std::string params = oss.str();
            params.pop_back(); // remove last ','
            stream_params_.push_back(std::move(params));
            oss.str("");
        }
    }

    INFRA_LOG_INFO("[aster] [subscribe_orderbook] [success], msg: establishing {} WebSocket connections",
                   stream_params_.size());
    for (size_t i = 0; i < stream_params_.size(); i++) {
        auto client = std::make_shared<WebSocketClient>(ioc_, ssl_ctx_, *this);
        wss_connections_.push_back(client);
        auto& conn = wss_connections_.back();
        conn->set_user_data(i); // index of wss_connections_
        conn->resolve_connect(wss_infos_.host, wss_infos_.port, wss_infos_.path);
    }
    return true;
}

void AsterMarketData::unsubscribe_orderbook() {
    for (auto& conn : wss_connections_) {
        if (conn->is_socket_open()) {
            conn->close();
        }
    }
    wss_connections_.clear();
    this->orderbook_handler_ = nullptr;
    INFRA_LOG_INFO("[aster] [unsubscribe_orderbook] [success]");
}

void AsterMarketData::fetch_pairs_info(ExPairInfoCallback cb) {
    if (pairs_info_path_.empty()) {
        cb(Errno::NotImplemented, {});
        return;
    }

    auto req = get_request_body(rest_host_, pairs_info_path_);
    rest_.send(req, [this, cb](HttpResponseBody& res) {
        std::string response = boost::beast::buffers_to_string(res.body().data());
        do {
            if (res.result() != HTTP_STATUS_OK) {
                break;
            }
            try {
                PARSE_JSON(response, doc);
                if (doc["symbols"].error() != simdjson::SUCCESS) {
                    break;
                }
                Currency currency = to_string(base_config_.settle_unit);
                parse_pairs_info(doc, currency);
                INFRA_LOG_INFO("[aster] [fetch_pairs_info] [success], size: {}", g_pairs_info_cache.size());
                cb(Errno::Ok, g_pairs_info_cache);
                return;
            } catch (const std::exception& ex) {
                INFRA_LOG_WARN("[aster] [fetch_pairs_info] [exception], msg: {}", ex.what());
            }
        } while (0);
        INFRA_LOG_WARN("[aster] [fetch_pairs_info] [fail], recv: {}", response);
        cb(extract_error_code(response), {});
    });
}

void AsterMarketData::fetch_funding_fee(const Symbol& symbol, FundingFeeCallback cb) {
    if (funding_fee_path_.empty()) {
        cb(Errno::NotImplemented, nullptr);
        return;
    }

    if (symbol.empty()) {
        cb(Errno::InvalidParams, nullptr);
        return;
    }

    std::string query = "symbol=" + transfer_from_infra_pair(symbol);
    auto req = get_request_body(rest_host_, funding_fee_path_, query);
    rest_.send(req, [cb](HttpResponseBody& res) {
        std::string response = boost::beast::buffers_to_string(res.body().data());
        do {
            if (res.result() != HTTP_STATUS_OK) {
                break;
            }
            try {
                PARSE_JSON(response, doc);
                INFRA_LOG_INFO("[aster] [fetch_funding_fee] [success], recv: {}", response);
                auto fee = parse_funding_fee(doc);
                cb(Errno::Ok, fee);
                return;
            } catch (const std::exception& ex) {
                INFRA_LOG_WARN("[aster] [fetch_funding_fee] [exception], msg: {}", ex.what());
            }
        } while (0);
        INFRA_LOG_WARN("[aster] [fetch_funding_fee] [fail], recv: {}", response);
        cb(extract_error_code(response), nullptr);
    });
}

Action AsterMarketData::on_connect(Wss* ws) {
    INFRA_LOG_INFO("[aster] [on_connect] [MarketData], msg: WebSocket connection ID: {}", ws->get_index());
    size_t index = ws->get_index();
    if (index < wss_connections_.size()) {
        subscribe(index);
    } else {
        INFRA_LOG_WARN("[aster] [on_connect] [fail], msg: invalid connection ID {}", index);
    }
    return Action::NONE;
}

Action AsterMarketData::on_ping(Wss* ws, std::string_view payload) {
    // INFRA_LOG_DEBUG("[aster] [on_ping] [MarketData], payload: {}", payload);
    ws->pong(std::string(payload));
    return Action::NONE;
}

Action AsterMarketData::on_pong(Wss* ws, std::string_view payload) {
    // INFRA_LOG_DEBUG("[aster] [on_pong] [MarketData], payload: {}", payload);
    return Action::NONE;
}

void AsterMarketData::on_close(Wss* ws) {
    size_t index = ws->get_index();
    INFRA_LOG_WARN("[aster] [on_close] [MarketData], msg: WebSocket connection has been closed, index: {}", index);
}

void AsterMarketData::on_error(Wss* ws, std::string_view err) {
    size_t index = ws->get_index();
    INFRA_LOG_WARN("[aster] [on_error] [MarketData], msg: WebSocket error occurred: {}, index: {}", err, index);
}

Action AsterMarketData::on_message(Wss* ws, std::string_view msg) {
    // INFRA_LOG_INFO("[aster] [on_message] [MarketData], msg: {}", msg);
    uint64_t recv_tsc = rdtsc();
    uint64_t recv_milli = time_get_now_milli();
    try {
        PARSE_JSON(msg, doc);
        if (doc["stream"].error() == simdjson::SUCCESS) {
            simdjson::dom::object data = doc["data"];
            std::string_view event = data["e"];
            if (event == "bookTicker") {
                on_message_bookticker(data, recv_tsc, recv_milli);
            } else if (event == "depthUpdate") {
                on_message_partial_depth(data, recv_tsc, recv_milli);
            }
        } else if (doc["id"].error() == simdjson::SUCCESS) {
            INFRA_LOG_INFO("[aster] [on_message] [success], msg: subscription response received, content: {}", msg);
            // NOTE：订阅响应不做处理
        }
    } catch (const std::exception& ex) {
        INFRA_LOG_WARN("[aster] [on_message] [exception], error: {}, msg: {}", ex.what(), msg);
    }
    return Action::RECEIVE;
}

void AsterMarketData::on_message_bookticker(const simdjson::dom::object& data, uint64_t recv_tsc, uint64_t recv_milli) {
    std::string_view symbol_text = data["s"];
    Symbol pair = transfer_to_infra_pair(symbol_text);
    Timestamp milli = data["E"];
    int64_t update_id = data["u"];

    std::string_view ask0_price_text = data["a"];
    std::string_view ask0_amount_text = data["A"];
    std::string_view bid0_price_text = data["b"];
    std::string_view bid0_amount_text = data["B"];

    std::list<Level> asks, bids;
    asks.emplace_back(str_to_float(ask0_price_text), str_to_float(ask0_amount_text));
    bids.emplace_back(str_to_float(bid0_price_text), str_to_float(bid0_amount_text));

    SpOrderBook orderbook = this->apply_orderbook_delta( pair, milli, asks, bids, , best_ask_price, best_ask_size, best_bid_price, best_bid_size);
    orderbook->recv_tsc = recv_tsc;
    orderbook->recv_milli = recv_milli;
    orderbook->parsed_tsc = rdtsc();
    this->dispatch_orderbook(std::move(orderbook));
}

void AsterMarketData::on_message_partial_depth(const simdjson::dom::object& data, uint64_t recv_tsc, uint64_t recv_milli) {
    std::string_view symbol_text = data["s"];
    Symbol pair = transfer_to_infra_pair(symbol_text);
    Timestamp milli = data["E"];
    int64_t last_update_id = data["pu"];

    double best_ask_price = 0.0;
    double best_ask_size = 0.0;
    double best_bid_price = 0.0;
    double best_bid_size = 0.0;
    std::list<Level> asks, bids;
    conj_orderbook_sides(data["a"], asks);
    conj_orderbook_sides(data["b"], bids);

    SpOrderBook orderbook = this->apply_orderbook_delta( pair, milli, asks, bids, , best_ask_price, best_ask_size, best_bid_price, best_bid_size);
    orderbook->recv_tsc = recv_tsc;
    orderbook->recv_milli = recv_milli;
    orderbook->parsed_tsc = rdtsc();
    this->dispatch_orderbook(std::move(orderbook));
}

void AsterMarketData::subscribe(size_t index) {
    std::string payload =
        R"({"id":")" + std::to_string(index) + R"(","method":"SUBSCRIBE","params":[)" + stream_params_[index] + R"(]})";
    INFRA_LOG_INFO("[aster] [batch_subscribe], connection {}, payload: {}", index, payload);
    wss_connections_[index]->send(std::move(payload));
}
} // namespace infra