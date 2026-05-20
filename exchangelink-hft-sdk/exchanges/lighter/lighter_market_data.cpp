#include "lighter_market_data.h"
#include <future>
using namespace infra::lighter;

namespace infra {
bool LighterMarketData::initialize() {
    auto& info = g_config_map[base_config_.to_str()];
    if (info.empty()) {
        INFRA_LOG_WARN("[lighter] [initialize] [fail], msg: {} {} {} not implemented",
                       to_string(base_config_.account_type), to_string(base_config_.address_type),
                       to_string(base_config_.settle_unit));
        return false;
    }

    rest_host_ = info[REST_HOST];
    pairs_info_path_ = info[PAIRS_INFO_PATH];
    funding_fee_path_ = info[FUNDING_FEE_PATH];
    wss_infos_ = {info[WSS_PUBLIC_HOST], info[WSS_PORT], info[WSS_PUBLIC_PATH]};
    INFRA_LOG_INFO("[lighter] [initialize] [MarketData], websocket endpoint: {} {} {}", wss_infos_.host,
                   wss_infos_.path, wss_infos_.port);
    // NOTE: subscribe_orderbook依赖pairs_info_cache，因此需要先获取pairs_info
    fetch_pairs_info_sync();
    return !g_pairs_info_cache.empty();
}

void LighterMarketData::shutdown() { unsubscribe_orderbook(); }

bool LighterMarketData::subscribe_orderbook(const Symbols& symbols, unsigned int depth, OrderbookCallback cb) {
    if (depth > 5) {
        INFRA_LOG_WARN("[lighter] [subscribe_orderbook] [fail], msg: unsupported depth level {}", depth);
        return false;
    }

    this->orderbook_handler_ = std::move(cb);
    const Symbols& sub_symbols = (!symbols.empty()) ? symbols : g_all_symbols;
    size_t total = sub_symbols.size();
    for (size_t i = 0; i < total; i++) {
        Symbol pair = transfer_from_infra_pair(sub_symbols[i]);
        auto it = g_pairs_info_cache.find(pair);
        if (it == g_pairs_info_cache.end()) {
            INFRA_LOG_WARN("[lighter] [subscribe_orderbook] [fail], msg: not found {} in cache", pair);
            continue;
        }

        stream_params_.push_back(it->second->alias);
    }

    size_t connection_nums = (stream_params_.size() / MAX_PAIRS_PER_WS_CONNECTION) + 1;
    INFRA_LOG_INFO("[lighter] [subscribe_orderbook] [success], msg: establishing {} WebSocket connections",
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

void LighterMarketData::unsubscribe_orderbook() {
    this->orderbook_handler_ = nullptr;
    for (auto& conn : wss_connections_) {
        if (conn->is_socket_open()) {
            conn->close();
        }
    }
    stream_params_.clear();
    wss_connections_.clear();
    INFRA_LOG_INFO("[lighter] [unsubscribe_orderbook] [success]");
}

void LighterMarketData::fetch_pairs_info(ExPairInfoCallback cb) {
    if (pairs_info_path_.empty()) {
        cb(Errno::NotImplemented, {});
        return;
    }

    auto req = get_request_body(rest_host_, pairs_info_path_);
    rest_.send(req, [this, cb](HttpResponseBody& res) {
        std::string msg = boost::beast::buffers_to_string(res.body().data());
        do {
            if (res.result() != HTTP_STATUS_OK) {
                break;
            }
            try {
                PARSE_JSON(msg, doc);
                if (doc["code"].get_int64() != LIGHTER_SUCCESS_CODE) {
                    break;
                }
                parse_pairs_info(doc);
                INFRA_LOG_INFO("[lighter] [fetch_pairs_info] [success], size: {}", g_pairs_info_cache.size());
                cb(Errno::Ok, g_pairs_info_cache);
                return;
            } catch (const std::exception& ex) {
                INFRA_LOG_WARN("[lighter] [fetch_pairs_info] [exception], msg: {}", ex.what());
            }
        } while (0);
        INFRA_LOG_WARN("[lighter] [fetch_pairs_info] [fail], recv: {}", msg);
        cb(extract_error_code(msg), {});
    });
}

void LighterMarketData::fetch_pairs_info_sync() {
    auto req = get_request_body(rest_host_, pairs_info_path_);
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
            parse_pairs_info(doc);
            INFRA_LOG_INFO("[lighter] [fetch_pairs_info] [success], size: {}", g_pairs_info_cache.size());
            return;
        } catch (const std::exception& ex) {
            INFRA_LOG_WARN("[lighter] [fetch_pairs_info] [exception], msg: {}", ex.what());
        }
    } while (0);
    INFRA_LOG_WARN("[lighter] [fetch_pairs_info] [fail], recv: {}", msg);
}

void LighterMarketData::fetch_funding_fee(const Symbol& symbol, FundingFeeCallback cb) {
    if (funding_fee_path_.empty()) {
        cb(Errno::NotImplemented, nullptr);
        return;
    }

    if (symbol.empty()) {
        cb(Errno::InvalidParams, nullptr);
        return;
    }

    auto req = get_request_body(rest_host_, funding_fee_path_);
    rest_.send(req, [cb, symbol](HttpResponseBody& res) {
        std::string msg = boost::beast::buffers_to_string(res.body().data());
        do {
            if (res.result() != HTTP_STATUS_OK) {
                break;
            }
            try {
                PARSE_JSON(msg, doc);
                if (doc["code"].get_int64() != LIGHTER_SUCCESS_CODE) {
                    break;
                }
                INFRA_LOG_INFO("[lighter] [fetch_funding_fee] [success]");
                auto fee = parse_funding_fee(doc, symbol);
                cb(Errno::Ok, fee);
                return;
            } catch (const std::exception& ex) {
                INFRA_LOG_WARN("[lighter] [fetch_funding_fee] [exception], msg: {}", ex.what());
            }
        } while (0);
        INFRA_LOG_WARN("[lighter] [fetch_funding_fee] [fail], recv: {}", msg);
        cb(extract_error_code(msg), nullptr);
    });
}

Action LighterMarketData::on_connect(Wss* ws) {
    size_t index = ws->get_index();
    INFRA_LOG_INFO("[lighter] [on_connect] [MarketData], msg: WebSocket connection established, index: {}", index);
    if (index >= wss_connections_.size()) {
        INFRA_LOG_WARN("[lighter] [on_connect] [fail], msg: invalid connection index {}, total connections: {}", index,
                       wss_connections_.size());
        return Action::NONE;
    }
    subscribe(index);
    return Action::NONE;
}

Action LighterMarketData::on_ping(Wss* ws, std::string_view payload) {
    // INFRA_LOG_DEBUG("[lighter] [on_ping] [MarketData], payload: {}", payload);
    ws->pong(std::string(payload));
    return Action::NONE;
}

Action LighterMarketData::on_pong(Wss* ws, std::string_view payload) {
    // INFRA_LOG_DEBUG("[lighter] [on_pong] [MarketData], payload: {}", payload);
    return Action::NONE;
}

void LighterMarketData::on_close(Wss* ws) {
    size_t index = ws->get_index();
    INFRA_LOG_WARN("[lighter] [on_close] [MarketData], msg: WebSocket connection has been closed, index: {}", index);
}

void LighterMarketData::on_error(Wss* ws, std::string_view err) {
    size_t index = ws->get_index();
    INFRA_LOG_WARN("[lighter] [on_error] [MarketData], msg: WebSocket error occurred: {}, index: {}", err, index);
}

Action LighterMarketData::on_message(Wss* ws, std::string_view msg) {
    // INFRA_LOG_DEBUG("[lighter] [on_message] [MarketData], msg: {}", msg);
    uint64_t recv_tsc = rdtsc();
    uint64_t recv_milli = time_get_now_milli();
    try {
        PARSE_JSON(msg, doc);
        if (doc["type"].error() == simdjson::SUCCESS) {
            std::string_view type = doc["type"];
            if (type.find("ticker") != std::string_view::npos) {
                on_message_bookticker(doc, recv_tsc, recv_milli);
            } else if (type == "ping") {
                return keep_ws_connection_alive(ws);
            } else if (type == "connected") {
                INFRA_LOG_INFO("[lighter] [on_message] [success], msg: {}", msg);
            } else {
                INFRA_LOG_WARN("[lighter] [on_message] unexpected msg: {}", msg);
            }
        } else {
            INFRA_LOG_WARN("[lighter] [on_message] unexcepted msg: {}", msg);
        }
    } catch (const std::exception& ex) {
        INFRA_LOG_WARN("[lighter] [on_message] [exception], error: {}, msg: {}", ex.what(), msg);
    }
    return Action::RECEIVE;
}

Action LighterMarketData::keep_ws_connection_alive(Wss* ws) {
    size_t index = ws->get_index();
    if (index < wss_connections_.size()) {
        wss_connections_[index]->send(R"({"type":"pong"})");
    } else {
        INFRA_LOG_WARN("[lighter] [keep_ws_connection_alive] [fail], msg: invalid connection index {}", index);
    }
    return Action::RECEIVE;
}

void LighterMarketData::subscribe(size_t index) {
    for (size_t i = 0; i < MAX_PAIRS_PER_WS_CONNECTION; i++) {
        size_t id = i + index * MAX_PAIRS_PER_WS_CONNECTION;
        if (id >= this->stream_params_.size()) {
            break;
        }
        std::string payload = fmt::format(R"({{"type":"subscribe","channel":"ticker/{}"}})", stream_params_[id]);
        INFRA_LOG_INFO("[lighter] [subscribe_orderbook], connection: {}, send: {}", index, payload);
        wss_connections_[index]->send(std::move(payload));
    }
}

void LighterMarketData::on_message_bookticker(const simdjson::dom::element& doc, uint64_t recv_tsc, uint64_t recv_milli) {
    std::string_view channel = doc["channel"];
    Timestamp milli = doc["timestamp"];

    std::string_view id = channel.substr(channel.find(':') + 1);
    int64_t market_id = std::stoi(std::string(id));
    Symbol pair = g_market_id_to_symbol[market_id];

    double best_ask_price = 0.0;
    double best_ask_size = 0.0;
    double best_bid_price = 0.0;
    double best_bid_size = 0.0;

    best_ask_price = str_to_float(doc["ticker"]["a"]["price"].get_string().value());
    best_ask_size = str_to_float(doc["ticker"]["a"]["size"].get_string().value());
    best_bid_price = str_to_float(doc["ticker"]["b"]["price"].get_string().value());
    best_bid_size = str_to_float(doc["ticker"]["b"]["size"].get_string().value());

    SpOrderBook orderbook = this->apply_orderbook_delta(pair, milli, best_ask_price,
                                                        best_ask_size, best_bid_price, best_bid_size);
    orderbook->recv_tsc = recv_tsc;
    orderbook->recv_milli = recv_milli;
    orderbook->parsed_tsc = rdtsc();
    this->dispatch_orderbook(std::move(orderbook));
}
} // namespace infra
