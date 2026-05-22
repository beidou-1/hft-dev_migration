#include "bitunix_market_data.h"
using namespace infra::bitunix;

namespace infra {
bool BitunixMarketData::initialize() {
    auto& info = g_config_map[base_config_.to_str()];
    if (info.empty()) {
        INFRA_LOG_WARN("[bitunix] [initialize] [fail], msg: {} {} {} not implemented",
                       to_string(base_config_.account_type), to_string(base_config_.address_type),
                       to_string(base_config_.settle_unit));
        return false;
    }

    rest_host_ = info[REST_HOST];
    pairs_info_path_ = info[PAIRS_INFO_PATH];
    funding_fee_path_ = info[FUNDING_FEE_PATH];

    wss_infos_ = {info[WSS_PUBLIC_HOST], info[WSS_PORT], info[WSS_PUBLIC_PATH]};
    INFRA_LOG_INFO("[bitunix] [initialize] [MarketData], websocket endpoint: {} {} {}", wss_infos_.host,
                   wss_infos_.path, wss_infos_.port);
    return true;
}

void BitunixMarketData::shutdown() { unsubscribe_orderbook(); }

bool BitunixMarketData::subscribe_orderbook(const Symbols& symbols, unsigned int depth, OrderbookCallback cb) {
    if (depth != 1 and depth != 5 and depth != 15) {
        INFRA_LOG_WARN("[bitunix] [subscribe_orderbook] [fail], msg: unsupported depth level {}", depth);
        return false;
    }

    std::string channel{};
    if (depth == 1) {
        channel = "depth_book1";
    } else if (depth == 5) {
        channel = "depth_book5";
    } else {
        channel = "depth_book15";
    }

    this->orderbook_handler_ = std::move(cb);
    const Symbols& sub_symbols = (!symbols.empty()) ? symbols : g_all_symbols;
    stream_params_.clear();
    size_t total = sub_symbols.size();
    for (size_t i = 0; i < total; i++) {
        std::string pair = transfer_from_infra_pair(sub_symbols[i]);
        size_t idx = i / MAX_PAIRS_PER_WS_CONNECTION;
        std::string param = fmt::format(R"({{"ch":"{}","symbol":"{}"}})", channel, pair);
        if (idx >= stream_params_.size()) {
            stream_params_.push_back(param);
        } else {
            stream_params_[idx].append(",").append(param);
        }
    }

    INFRA_LOG_INFO("[bitunix] [subscribe_orderbook], msg: establishing {} WebSocket connections",
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

void BitunixMarketData::unsubscribe_orderbook() {
    this->orderbook_handler_ = nullptr;
    for (auto& conn : wss_connections_) {
        if (conn->is_socket_open()) {
            conn->close();
        }
    }
    stream_params_.clear();
    wss_connections_.clear();
    INFRA_LOG_INFO("[bitunix] [unsubscribe_orderbook] [success]");
}

void BitunixMarketData::fetch_pairs_info(ExPairInfoCallback cb) {
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
                if (doc["code"].get_int64() != SUCCESS_CODE) {
                    break;
                }
                Currency currency = to_string(base_config_.settle_unit);
                parse_pairs_info(doc, currency);
                INFRA_LOG_INFO("[bitunix] [fetch_pairs_info] [success], size: {}", g_pairs_info_cache.size());
                cb(Errno::Ok, g_pairs_info_cache);
                return;
            } catch (const std::exception& ex) {
                INFRA_LOG_WARN("[bitunix] [fetch_pairs_info] [exception], msg: {}", ex.what());
            }
        } while (0);
        INFRA_LOG_WARN("[bitunix] [fetch_pairs_info] [fail], recv: {}", response);
        cb(extract_error_code(response), {});
    });
}

void BitunixMarketData::fetch_funding_fee(const Symbol& symbol, FundingFeeCallback cb) {
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
                if (doc["code"].get_int64() != SUCCESS_CODE) {
                    break;
                }
                INFRA_LOG_INFO("[bitunix] [fetch_funding_fee] [success], recv: {}", response);
                auto fee = parse_funding_fee(doc);
                cb(Errno::Ok, fee);
                return;
            } catch (const std::exception& ex) {
                INFRA_LOG_WARN("[bitunix] [fetch_funding_fee] [exception], msg: {}", ex.what());
            }
        } while (0);
        INFRA_LOG_WARN("[bitunix] [fetch_funding_fee] [fail], recv: {}", response);
        cb(extract_error_code(response), nullptr);
    });
}

Action BitunixMarketData::on_connect(Wss* ws) {
    size_t index = ws->get_index();
    INFRA_LOG_INFO("[bitunix] [on_connect] [MarketData], msg: WebSocket connection established, index: {}", index);
    if (index < wss_connections_.size()) {
        keep_ws_connection_alive(index);
        subscribe(index);
    } else {
        INFRA_LOG_WARN("[bitunix] [on_connect] [MarketData], msg: invalid connection index: {}", index);
    }
    return Action::NONE;
}

Action BitunixMarketData::on_ping(Wss* ws, std::string_view payload) {
    // INFRA_LOG_INFO("[bitunix] [on_ping] [MarketData], payload: {}", payload);
    ws->pong(std::string(payload));
    return Action::NONE;
}

Action BitunixMarketData::on_pong(Wss* ws, std::string_view payload) {
    // INFRA_LOG_INFO("[bitunix] [on_pong] [MarketData], payload: {}", payload);
    return Action::NONE;
}

void BitunixMarketData::on_close(Wss* ws) {
    size_t index = ws->get_index();
    INFRA_LOG_WARN("[bitunix] [on_close] [MarketData], msg: WebSocket connection has been closed, index: {}", index);
}

void BitunixMarketData::on_error(Wss* ws, std::string_view err) {
    size_t index = ws->get_index();
    INFRA_LOG_WARN("[bitunix] [on_error] [MarketData], msg: WebSocket error occurred: {}, index: {}", err, index);
}

Action BitunixMarketData::on_message(Wss* ws, std::string_view msg) {
    // INFRA_LOG_INFO("[bitunix] [on_message] [MarketData], msg: {}", msg);
    uint64_t recv_tsc = rdtsc();
    uint64_t recv_milli = time_get_now_milli();
    try {
        PARSE_JSON(msg, doc);
        if (doc["ch"].error() == simdjson::SUCCESS) {
            std::string_view ch = doc["ch"];
            if (ch.find("depth_book") != std::string_view::npos) {
                std::int64_t ts = doc["ts"];
                std::string_view symbol = doc["symbol"];
                on_message_bookticker(doc["data"], symbol, ts, recv_tsc, recv_milli);
            } else {
                INFRA_LOG_WARN("[bitunix] [on_message] unexpected msg: {}", msg);
            }
        } else if (doc["op"].error() == simdjson::SUCCESS) {
            std::string_view op = doc["op"];
            if (op == "connect") {
                bool result = doc["data"]["result"];
                if (result) {
                    INFRA_LOG_INFO("[bitunix] [MarketData] [success], msg: {}", msg);
                } else {
                    INFRA_LOG_WARN("[bitunix] [MarketData] [fail], msg: {}", msg);
                }
            } else if (op == "ping") {
                // ignore
            } else {
                INFRA_LOG_WARN("[bitunix] [on_message], unexpected msg: {}", msg);
            }
        } else {
            INFRA_LOG_WARN("[bitunix] [on_message], unexpected msg: {}", msg);
        }
    } catch (const std::exception& ex) {
        INFRA_LOG_WARN("[bitunix] [on_message] [exception], error: {}, msg: {}", ex.what(), msg);
    }
    return Action::RECEIVE;
}

void BitunixMarketData::keep_ws_connection_alive(size_t index) {
    std::string msg = fmt::format(R"({{"op":"ping","ping":{}}})", time_get_now_sec());
    wss_connections_[index]->start_ping_pong(msg, 10);
}

void BitunixMarketData::subscribe(size_t index) {
    std::string topic = stream_params_[index];
    std::string msg = fmt::format(R"({{"op":"subscribe","args":[{}]}})", topic);
    INFRA_LOG_INFO("[bitunix] [subscribe_orderbook], connection index: {}, send: {}", index, msg);
    wss_connections_[index]->send(std::move(msg));
}

void BitunixMarketData::on_message_bookticker(const simdjson::dom::object& data, std::string_view symbol,
                                              std::int64_t ts, uint64_t recv_tsc, uint64_t recv_milli) {
    Symbol pair = transfer_to_infra_pair(symbol);
    double denomination = get_denomination_value(pair);                                            
    double best_ask_price = 0.0;
    double best_ask_size = 0.0;
    double best_bid_price = 0.0;
    double best_bid_size = 0.0;

    for (auto&& items : data["a"]) {
        auto it = items.begin();
        best_ask_price = str_to_float(std::string_view(*it));
        ++it;
        best_ask_size = str_to_float(std::string_view(*it)) * denomination;
        break;
    }

    for (auto&& items : data["b"]) {
        auto it = items.begin();
        best_bid_price = str_to_float(std::string_view(*it));
        ++it;
        best_bid_size = str_to_float(std::string_view(*it)) * denomination;
        break;
    }

    Timestamp milli = ts;

    SpOrderBook orderbook = this->apply_orderbook_delta( pair, milli, best_ask_price, best_ask_size, best_bid_price, best_bid_size);
    orderbook->recv_tsc = recv_tsc;
    orderbook->recv_milli = recv_milli;
    orderbook->parsed_tsc = rdtsc();
    this->dispatch_orderbook(std::move(orderbook));
}
} // namespace infra