
#include "kucoin_market_data.h"
using namespace infra::kucoin;

namespace infra {
bool KucoinMarketData::initialize() {
    auto& info = g_config_map[base_config_.to_str()];
    if (info.empty()) {
        INFRA_LOG_WARN("[kucoin] [initialize] [fail], msg: {} {} {} {} not implemented",
                       to_string(base_config_.account_type), to_string(base_config_.address_type),
                       to_string(base_config_.account_mode), to_string(base_config_.settle_unit));
        return false;
    }

    rest_host_ = info[REST_HOST];
    pairs_info_path_ = info[PAIRS_INFO_PATH];
    funding_fee_path_ = info[FUNDING_FEE_PATH];

    // if (g_account_mode == AccountMode::CLASSIC) {
    //     std::string public_token = get_public_token();
    //     std::string public_path = fmt::format(info[WSS_PUBLIC_PATH], public_token, time_get_now_micro());
    //     wss_infos_ = {info[WSS_PUBLIC_HOST], info[WSS_PORT], public_path};
    // } else if (g_account_mode == AccountMode::UNIFIED) {
    wss_infos_ = {info[WSS_PUBLIC_HOST], info[WSS_PORT], info[WSS_PUBLIC_PATH]};
    // }

    INFRA_LOG_INFO("[kucoin] [initialize] [MarketData], websocket endpoint: {} {} {}", wss_infos_.host, wss_infos_.path,
                   wss_infos_.port);
    return true;
}

void KucoinMarketData::shutdown() { unsubscribe_orderbook(); }

bool KucoinMarketData::subscribe_orderbook(const Symbols& symbols, unsigned int depth, OrderbookCallback cb) {
    // if (g_account_mode == AccountMode::CLASSIC) {
    //     return subscribe_classic_orderbook(symbols, depth, cb);
    // } else if (g_account_mode == AccountMode::UNIFIED) {
    return subscribe_unified_orderbook(symbols, depth, cb);
    // }
    // return true;
}

void KucoinMarketData::unsubscribe_orderbook() {
    this->orderbook_handler_ = nullptr;
    for (auto& conn : wss_connections_) {
        if (conn->is_socket_open()) {
            conn->close();
        }
    }
    stream_params_.clear();
    wss_connections_.clear();
    INFRA_LOG_INFO("[kucoin] [unsubscribe_orderbook] [success]");
}

void KucoinMarketData::fetch_pairs_info(ExPairInfoCallback cb) {
    // if (g_account_mode == AccountMode::CLASSIC) {
    //     fetch_classic_pairs_info(cb);
    // } else if (g_account_mode == AccountMode::UNIFIED) {
    fetch_unified_pairs_info(cb);
    // }
}

void KucoinMarketData::fetch_funding_fee(const Symbol& symbol, FundingFeeCallback cb) {
    // if (g_account_mode == AccountMode::CLASSIC) {
    //     fetch_classic_funding_fee(symbol, cb);
    // } else if (g_account_mode == AccountMode::UNIFIED) {
    fetch_unified_funding_fee(symbol, cb);
    // }
}

Action KucoinMarketData::on_connect(Wss* ws) {
    // if (g_account_mode == AccountMode::CLASSIC) {
    //     return on_classic_connect(ws);
    // } else if (g_account_mode == AccountMode::UNIFIED) {
    return on_unified_connect(ws);
    // }
    return Action::NONE;
}

Action KucoinMarketData::on_ping(Wss* ws, std::string_view payload) {
    // INFRA_LOG_DEBUG("[kucoin] [on_ping] [MarketData], payload: {}", payload);
    return Action::NONE;
}

Action KucoinMarketData::on_pong(Wss* ws, std::string_view payload) {
    // INFRA_LOG_DEBUG("[kucoin] [on_pong] [MarketData], payload: {}", payload);
    return Action::NONE;
}

void KucoinMarketData::on_close(Wss* ws) {
    size_t index = ws->get_index();
    INFRA_LOG_WARN("[kucoin] [on_close] [MarketData], msg: WebSocket connection has been closed, index: {}", index);
}

void KucoinMarketData::on_error(Wss* ws, std::string_view err) {
    size_t index = ws->get_index();
    INFRA_LOG_WARN("[kucoin] [on_error] [MarketData], msg: WebSocket error occurred: {}, index: {}", err, index);
}

Action KucoinMarketData::on_message(Wss* ws, std::string_view msg) {
    // if (g_account_mode == AccountMode::CLASSIC) {
    //     return on_classic_message(ws, msg);
    // } else if (g_account_mode == AccountMode::UNIFIED) {
    return on_unified_message(ws, msg);
    // }
    return Action::RECEIVE;
}

/* unified */
Action KucoinMarketData::on_unified_connect(Wss* ws) {
    size_t index = ws->get_index();
    INFRA_LOG_INFO("[kucoin] [on_connect] [MarketData], msg: WebSocket connection established, index: {}", index);
    if (index < wss_connections_.size()) {
        auto& websocket_client = wss_connections_[index];
        keep_ws_connection_alive(*websocket_client);
        subscribe_unified(index);
    } else {
        INFRA_LOG_WARN("[kucoin] [on_connect] [MarketData], msg: invalid connection index: {}", index);
    }
    return Action::NONE;
}

void KucoinMarketData::subscribe_unified(size_t index) {
    size_t delay_s = index + 2; // NOTE：延迟订阅，提高成功率
    auto timer = std::make_shared<boost::asio::steady_timer>(ioc_, std::chrono::seconds(delay_s));
    timer->async_wait([this, index, timer](const boost::system::error_code& ec) {
        for (size_t i = 0; i < MAX_PAIRS_PER_WS_CONNECTION; i++) {
            size_t id = i + index * MAX_PAIRS_PER_WS_CONNECTION;
            if (id >= this->stream_params_.size()) {
                break;
            }
            std::string payload =
                fmt::format(R"({{"id":"{}","action":"subscribe",{}}})", time_get_now_micro(), this->stream_params_[id]);
            INFRA_LOG_INFO("[kucoin] [subscribe_orderbook], connection index: {}, send: {}", index, payload);
            wss_connections_[index]->send(std::move(payload));
        }
    });
}

void KucoinMarketData::on_message_unified_bookticker(const simdjson::dom::object& data) {
    std::string_view symbol = data["s"];
    Symbol pair = transfer_to_infra_pair(symbol);
    double denomination = get_denomination_value(pair); // 合约张数转币数

    std::list<Level> asks, bids;
    asks.emplace_back(bfloat{std::string_view(data["bestAskPrice"])},
                      bfloat{std::to_string(data["bestAskSize"].get_int64())} * denomination);
    bids.emplace_back(bfloat{std::string_view(data["bestBidPrice"])},
                      bfloat{std::to_string(data["bestBidSize"].get_int64())} * denomination);
    Timestamp milli = data["P"].get_int64() / 1'000'000L;

    SpOrderBook orderbook = this->apply_orderbook_delta(true, pair, milli, asks, bids);
    this->dispatch_orderbook(std::move(orderbook));
}

void KucoinMarketData::on_message_unified_orderbook(const simdjson::dom::object& data) {
    std::string_view symbol = data["s"];
    Symbol pair = transfer_to_infra_pair(symbol);
    bfloat denomination = get_denomination_value(pair); // 合约张数转币数

    std::list<Level> asks, bids;
    conj_orderbook_sides(data["a"], asks, denomination);
    conj_orderbook_sides(data["b"], bids, denomination);
    Timestamp milli = data["M"].get_int64() / 1'000'000L;

    SpOrderBook orderbook = this->apply_orderbook_delta(true, pair, milli, asks, bids);
    this->dispatch_orderbook(std::move(orderbook));
}

bool KucoinMarketData::subscribe_unified_orderbook(const Symbols& symbols, unsigned int depth, OrderbookCallback cb) {
    if (depth != 1 and depth != 5 and depth != 50) {
        INFRA_LOG_WARN("[kucoin] [subscribe_orderbook] [fail], msg: unsupported depth level {}", depth);
        return false;
    }

    this->orderbook_handler_ = std::move(cb);
    const Symbols& sub_symbols = (!symbols.empty()) ? symbols : g_all_symbols;
    stream_params_.clear();
    size_t total = sub_symbols.size();
    for (size_t i = 0; i < total; i++) {
        std::string pair = transfer_from_infra_pair(sub_symbols[i]);
        std::string param =
            fmt::format(R"("channel":"obu","tradeType":"FUTURES","symbol":"{}","depth":"{}")", pair, depth);
        stream_params_.emplace_back(param);
    }

    size_t connection_nums = (stream_params_.size() / MAX_PAIRS_PER_WS_CONNECTION) + 1;
    INFRA_LOG_INFO("[kucoin] [subscribe_orderbook], msg: subscribing {} WebSocket connections", connection_nums);
    for (size_t i = 0; i < connection_nums; i++) {
        auto client = std::make_shared<WebSocketClient>(ioc_, ssl_ctx_, *this);
        wss_connections_.push_back(client);
        auto& conn = wss_connections_.back();
        conn->set_user_data(i); // index of wss_connections_
        conn->resolve_connect(wss_infos_.host, wss_infos_.port, wss_infos_.path);
    }
    return true;
}

Action KucoinMarketData::on_unified_message(Wss* ws, std::string_view msg) {
    // INFRA_LOG_INFO("[kucoin] [on_message] [MarketData], msg: {}", msg);
    try {
        PARSE_JSON(msg, doc);
        if (doc["T"].error() == simdjson::SUCCESS) {
            std::string_view topic = doc["T"];
            if (topic.find("obu.FUTURES") != std::string_view::npos) {
                std::string_view type = doc["t"];
                if (type == "snapshot") {
                    on_message_unified_orderbook(doc["d"]);
                }
            }
        } else if (doc["message"].error() == simdjson::SUCCESS) {
            std::string_view message = doc["message"];
            if (message == "welcome") {
                INFRA_LOG_INFO("[kucoin] [{}] [success], recv: {}", message, msg);
            } else {
                INFRA_LOG_WARN("[kucoin] [on_message], unexpected msg: {}", msg);
            }
        } else if (doc["type"].error() == simdjson::SUCCESS) {
            std::string_view type = doc["type"];
            if (type == "pong") {
                // pass
            } else {
                INFRA_LOG_WARN("[kucoin] [on_message], unexpected msg: {}", msg);
            }
        } else if (doc["result"].error() == simdjson::SUCCESS) {
            bool result = doc["result"];
            if (result) {
                // pass
            } else {
                INFRA_LOG_WARN("[kucoin] [on_message], unexpected msg: {}", msg);
            }
        } else {
            INFRA_LOG_WARN("[kucoin] [on_message], unexpected msg: {}", msg);
        }
    } catch (const std::exception& ex) {
        INFRA_LOG_WARN("[kucoin] [on_message] [MarketData], exception error: {}, msg: {}", ex.what(), msg);
    }
    return Action::RECEIVE;
}

void KucoinMarketData::fetch_unified_pairs_info(ExPairInfoCallback cb) {
    if (pairs_info_path_.empty()) {
        cb(Errno::NotImplemented, {});
        return;
    }

    std::string query = "tradeType=FUTURES";
    auto req = get_request_body(rest_host_, pairs_info_path_, query);
    rest_.send(req, [this, cb](HttpResponseBody& res) {
        std::string response = boost::beast::buffers_to_string(res.body().data());
        do {
            if (res.result() != HTTP_STATUS_OK) {
                break;
            }
            try {
                PARSE_JSON(response, doc);
                if (doc["code"].get_string()->compare(KUCOIN_SUCCESS_CODE) != 0) {
                    break;
                }
                Currency currency = "USDT";
                parse_unified_pairs_info(doc, currency);
                INFRA_LOG_INFO("[kucoin] [fetch_pairs_info] [success], size: {}", g_pairs_info_cache.size());
                cb(Errno::Ok, g_pairs_info_cache);
                return;
            } catch (const std::exception& ex) {
                INFRA_LOG_WARN("[kucoin] [fetch_pairs_info] [exception], msg: {}", ex.what());
            }
        } while (0);
        INFRA_LOG_WARN("[kucoin] [fetch_pairs_info] [fail], recv: {}", response);
        cb(extract_error_code(response), {});
    });
}

void KucoinMarketData::fetch_unified_funding_fee(const Symbol& symbol, FundingFeeCallback cb) {
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
    rest_.send(req, [symbol, cb](HttpResponseBody& res) {
        std::string response = boost::beast::buffers_to_string(res.body().data());
        do {
            if (res.result() != HTTP_STATUS_OK) {
                break;
            }
            try {
                PARSE_JSON(response, doc);
                if (doc["code"].get_string()->compare(KUCOIN_SUCCESS_CODE) != 0) {
                    break;
                }
                INFRA_LOG_INFO("[kucoin] [fetch_funding_fee] [success], recv: {}", response);
                auto fee = parse_unified_funding_fee(doc, symbol);
                cb(Errno::Ok, fee);
                return;
            } catch (const std::exception& ex) {
                INFRA_LOG_WARN("[kucoin] [fetch_funding_fee] [exception], msg: {}", ex.what());
            }
        } while (0);
        INFRA_LOG_WARN("[kucoin] [fetch_funding_fee] [fail], recv: {}", response);
        cb(extract_error_code(response), nullptr);
    });
}

/* classic */
std::string KucoinMarketData::get_public_token() {
    auto req = get_request_body_by_post(rest_host_, "/api/v1/bullet-public", "");
    boost::beast::error_code ec;
    std::string response = rest_.sync_send(req, ec);
    do {
        if (ec) {
            break;
        }
        try {
            PARSE_JSON(response, doc);
            if (doc["code"].get_string()->compare(KUCOIN_SUCCESS_CODE) != 0) {
                break;
            }
            INFRA_LOG_INFO("[kucoin] [get_public_token] [success], recv: {}", response);

            return std::string(doc["data"]["token"]);
        } catch (const std::exception& ex) {
            INFRA_LOG_WARN("[kucoin] [get_public_token] [exception], msg: {}", ex.what());
        }
    } while (0);
    INFRA_LOG_WARN("[kucoin] [get_public_token] [fail], recv: {}", response);
    return "";
}

Action KucoinMarketData::on_classic_connect(Wss* ws) {
    size_t index = ws->get_index();
    INFRA_LOG_INFO("[kucoin] [on_connect] [MarketData], msg: WebSocket connection established, index: {}", index);
    if (index < wss_connections_.size()) {
        auto& websocket_client = wss_connections_[index];
        keep_ws_connection_alive(*websocket_client);
        subscribe_classic(index);
    } else {
        INFRA_LOG_WARN("[kucoin] [on_connect] [MarketData], msg: invalid connection index: {}", index);
    }
    return Action::NONE;
}

void KucoinMarketData::subscribe_classic(size_t index) {
    std::string topic = stream_params_[index];
    std::string msg =
        fmt::format(R"({{"id":{},"type":"subscribe","topic":"{}","response":true}})", time_get_now_micro(), topic);
    INFRA_LOG_INFO("[kucoin] [subscribe_orderbook], connection index: {}, send: {}", index, msg);
    wss_connections_[index]->send(std::move(msg));
}

void KucoinMarketData::on_message_classic_bookticker(const simdjson::dom::object& data) {
    std::string_view symbol = data["symbol"];
    Symbol pair = transfer_to_infra_pair(symbol);
    bfloat denomination = get_denomination_value(pair); // 合约张数转币数

    std::list<Level> asks, bids;
    asks.emplace_back(bfloat{std::string_view(data["bestAskPrice"])},
                      bfloat{std::to_string(data["bestAskSize"].get_int64())} * denomination);
    bids.emplace_back(bfloat{std::string_view(data["bestBidPrice"])},
                      bfloat{std::to_string(data["bestBidSize"].get_int64())} * denomination);
    Timestamp milli = data["ts"].get_int64() / 1'000'000L;

    SpOrderBook orderbook = this->apply_orderbook_delta(true, pair, milli, asks, bids);
    this->dispatch_orderbook(std::move(orderbook));
}

void KucoinMarketData::on_message_classic_orderbook(const simdjson::dom::object& data, const Symbol& symbol) {
    Symbol pair = transfer_to_infra_pair(symbol);
    bfloat denomination = get_denomination_value(pair); // 合约张数转币数

    std::list<Level> asks, bids;
    conj_orderbook_sides_mix(data["asks"], asks, denomination);
    conj_orderbook_sides_mix(data["bids"], bids, denomination);
    Timestamp milli = data["ts"].get_int64() / 1'000'000L;

    SpOrderBook orderbook = this->apply_orderbook_delta(true, pair, milli, asks, bids);
    this->dispatch_orderbook(std::move(orderbook));
}

bool KucoinMarketData::subscribe_classic_orderbook(const Symbols& symbols, unsigned int depth, OrderbookCallback cb) {
    if (depth != 1 and depth != 5 and depth != 50) {
        INFRA_LOG_WARN("[kucoin] [subscribe_orderbook] [fail], msg: unsupported depth level {}", depth);
        return false;
    }

    this->orderbook_handler_ = std::move(cb);
    const Symbols& sub_symbols = (!symbols.empty()) ? symbols : g_all_symbols;
    stream_params_.clear();
    size_t total = sub_symbols.size();
    for (size_t i = 0; i < total; i++) {
        std::string pair = transfer_from_infra_pair(sub_symbols[i]);
        size_t idx = i / MAX_PAIRS_PER_WS_CONNECTION;
        if (idx >= stream_params_.size()) {
            std::string topic;
            if (depth == 1) {
                topic = fmt::format("/contractMarket/tickerV2:{}", pair);
            } else {
                topic = fmt::format("/contractMarket/level2Depth{}:{}", depth, pair);
            }
            stream_params_.emplace_back(topic);
        } else {
            stream_params_[idx].append(",").append(pair);
        }
    }

    INFRA_LOG_INFO("[kucoin] [subscribe_orderbook], msg: subscribing {} WebSocket connections", stream_params_.size());
    for (size_t i = 0; i < stream_params_.size(); i++) {
        auto client = std::make_shared<WebSocketClient>(ioc_, ssl_ctx_, *this);
        wss_connections_.push_back(client);
        auto& conn = wss_connections_.back();
        conn->set_user_data(i); // index of wss_connections_
        conn->resolve_connect(wss_infos_.host, wss_infos_.port, wss_infos_.path);
    }
    return true;
}

Action KucoinMarketData::on_classic_message(Wss* ws, std::string_view msg) {
    // INFRA_LOG_INFO("[kucoin] [on_message] [MarketData], msg: {}", msg);
    try {
        PARSE_JSON(msg, doc);
        if (doc["topic"].error() == simdjson::SUCCESS) {
            std::string_view topic = doc["topic"];
            if (topic.find("/contractMarket/tickerV2") != std::string_view::npos) {
                simdjson::dom::object data = doc["data"];
                on_message_classic_bookticker(data);
            } else if (topic.find("/contractMarket/level2Depth") != std::string_view::npos) {
                simdjson::dom::object data = doc["data"];
                size_t pos = topic.find(':');
                auto symbol = std::string(topic.substr(pos + 1));
                on_message_classic_orderbook(data, symbol);
            }
        } else if (doc["type"].error() == simdjson::SUCCESS) {
            std::string_view type = doc["type"];
            if (type == "pong") {
                // ignore
            } else if (type == "welcome" or type == "ack") {
                INFRA_LOG_INFO("[kucoin] [{}] [success], recv: {}", type, msg);
            } else {
                INFRA_LOG_WARN("[kucoin] [on_message], unexpected msg: {}", msg);
            }
        }
    } catch (const std::exception& ex) {
        INFRA_LOG_WARN("[kucoin] [on_message] [MarketData], exception error: {}, msg: {}", ex.what(), msg);
    }
    return Action::RECEIVE;
}

void KucoinMarketData::fetch_classic_pairs_info(ExPairInfoCallback cb) {
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
                if (doc["code"].get_string()->compare(KUCOIN_SUCCESS_CODE) != 0) {
                    break;
                }
                Currency currency = "USDT";
                parse_classic_pairs_info(doc, currency);
                INFRA_LOG_INFO("[kucoin] [fetch_pairs_info] [success], size: {}", g_pairs_info_cache.size());
                cb(Errno::Ok, g_pairs_info_cache);
                return;
            } catch (const std::exception& ex) {
                INFRA_LOG_WARN("[kucoin] [fetch_pairs_info] [exception], msg: {}", ex.what());
            }
        } while (0);
        INFRA_LOG_WARN("[kucoin] [fetch_pairs_info] [fail], recv: {}", response);
        cb(extract_error_code(response), {});
    });
}

void KucoinMarketData::fetch_classic_funding_fee(const Symbol& symbol, FundingFeeCallback cb) {
    if (funding_fee_path_.empty()) {
        cb(Errno::NotImplemented, nullptr);
        return;
    }

    if (symbol.empty()) {
        cb(Errno::InvalidParams, nullptr);
        return;
    }

    Symbol exchange_symbol = transfer_from_infra_pair(symbol);
    std::string path = fmt::format(funding_fee_path_, exchange_symbol);
    auto req = get_request_body(rest_host_, path);
    rest_.send(req, [symbol, cb](HttpResponseBody& res) {
        std::string response = boost::beast::buffers_to_string(res.body().data());
        do {
            if (res.result() != HTTP_STATUS_OK) {
                break;
            }
            try {
                PARSE_JSON(response, doc);
                if (doc["code"].get_string()->compare(KUCOIN_SUCCESS_CODE) != 0) {
                    break;
                }
                INFRA_LOG_INFO("[kucoin] [fetch_funding_fee] [success], recv: {}", response);
                auto fee = parse_classic_funding_fee(doc, symbol);
                cb(Errno::Ok, fee);
                return;
            } catch (const std::exception& ex) {
                INFRA_LOG_WARN("[kucoin] [fetch_funding_fee] [exception], msg: {}", ex.what());
            }
        } while (0);
        INFRA_LOG_WARN("[kucoin] [fetch_funding_fee] [fail], recv: {}", response);
        cb(extract_error_code(response), nullptr);
    });
}

} // namespace infra