#include "hbg_market_data.h"
using namespace infra::hbg;

namespace infra {
bool HbgMarketData::initialize() {
    auto& info = g_config_map[base_config_.to_str()];
    if (info.empty()) {
        INFRA_LOG_WARN("[hbg] [initialize] [fail], msg: {} {} {} not implemented", to_string(base_config_.account_type),
                       to_string(base_config_.address_type), to_string(base_config_.settle_unit));
        return false;
    }

    rest_host_ = info[REST_HOST];
    pairs_info_path_ = info[PAIRS_INFO_PATH];
    funding_fee_path_ = info[FUNDING_FEE_PATH];
    wss_infos_ = {info[WSS_PUBLIC_HOST], info[WSS_PORT], info[WSS_PUBLIC_PATH]};
    INFRA_LOG_INFO("[hbg] [initialize] [MarketData], websocket endpoint: {} {} {}", wss_infos_.host, wss_infos_.path,
                   wss_infos_.port);
    return true;
}

void HbgMarketData::shutdown() { unsubscribe_orderbook(); }

bool HbgMarketData::subscribe_orderbook(const Symbols& symbols, unsigned int depth, OrderbookCallback cb) {
    return this->subscribe_bookticker(symbols, cb);
}

void HbgMarketData::unsubscribe_orderbook() {
    this->orderbook_handler_ = nullptr;
    for (auto& conn : wss_connections_) {
        if (conn->is_socket_open()) {
            conn->close();
        }
    }
    stream_params_.clear();
    wss_connections_.clear();
    INFRA_LOG_INFO("[hbg] [unsubscribe_orderbook] [success]");
}

bool HbgMarketData::subscribe_bookticker(const Symbols& symbols, OrderbookCallback cb) {
    constexpr size_t MAX_STREAMS_PER_WSS_CONNECTION = 80;

    this->orderbook_handler_ = std::move(cb);
    const Symbols& sub_symbols = (!symbols.empty()) ? symbols : g_all_symbols;
    stream_params_.clear();
    std::vector<std::string> current_batch;
    size_t total = sub_symbols.size();
    for (size_t i = 0; i < total; ++i) {
        std::string symbol_upper = transfer_from_infra_pair(sub_symbols[i]);
        std::string req_id = std::to_string(generate_req_id());
        current_batch.push_back(fmt::format(R"({{"sub":"market.{}.bbo","id":"{}"}})", symbol_upper, req_id));
        if (current_batch.size() >= MAX_STREAMS_PER_WSS_CONNECTION || i == total - 1) {
            stream_params_.push_back(std::move(current_batch));
            current_batch.clear();
        }
    }

    INFRA_LOG_INFO("[hbg] [subscribe_bookticker], msg: establishing {} WebSocket connections", stream_params_.size());
    for (size_t i = 0; i < stream_params_.size(); i++) {
        auto client = std::make_shared<WebSocketClient>(ioc_, ssl_ctx_, *this);
        wss_connections_.push_back(client);
        auto& conn = wss_connections_.back();
        conn->set_user_data(i);
        conn->resolve_connect(wss_infos_.host, wss_infos_.port, wss_infos_.path);
    }
    return true;
}

void HbgMarketData::fetch_pairs_info(ExPairInfoCallback cb) {
    if (pairs_info_path_.empty()) {
        cb(Errno::NotImplemented, {});
        return;
    }

    auto req = get_request_body(rest_host_, pairs_info_path_);
    client_.send(req, [this, cb](HttpResponseBody& res) {
        std::string msg = boost::beast::buffers_to_string(res.body().data());
        handle_rest_response(
            res, msg, "fetch_pairs_info",
            [&](auto& doc) {
                if (!doc["data"].is_array())
                    return false;
                Currency currency = to_string(base_config_.settle_unit);
                parse_pairs_info(doc, currency);
                INFRA_LOG_INFO("[hbg] [fetch_pairs_info] [success], size: {}", g_pairs_info_cache.size());
                cb(Errno::Ok, g_pairs_info_cache);
                return true;
            },
            [&]() { cb(extract_error_code(msg), {}); });
    });
}

bool HbgMarketData::send_ws_request(const std::shared_ptr<WebSocketClient>& client, const std::string& content,
                                    const std::string& name) {
    if (client->is_socket_open()) {
        client->send(content);
        if (name != "ping") {
            INFRA_LOG_INFO("[hbg] [{}], send: {}", name, content);
        }
        return true;
    } else {
        INFRA_LOG_WARN("[hbg] [{}] [fail], msg: WebSocket not connected", name);
        return false;
    }
}

void HbgMarketData::fetch_funding_fee(const Symbol& symbol, FundingFeeCallback cb) {
    if (symbol.empty()) {
        cb(Errno::InvalidParams, nullptr);
        return;
    }

    Symbol pair = transfer_from_infra_pair(symbol);
    std::string query = "contract_code=" + pair;
    auto req = get_request_body(rest_host_, funding_fee_path_, query);
    client_.send(req, [this, symbol, cb](HttpResponseBody& res) {
        std::string msg = boost::beast::buffers_to_string(res.body().data());
        handle_rest_response(
            res, msg, "fetch_funding_fee",
            [&](auto& doc) {
                if (doc["status"].get_string().value() != "ok")
                    return false;
                INFRA_LOG_INFO("[hbg] [fetch_funding_fee] [success], recv: {}", msg);
                auto funding_fee = parse_funding_fee(doc, symbol);
                cb(Errno::Ok, funding_fee);
                return true;
            },
            [&]() { cb(extract_error_code(msg), nullptr); });
    });
}

Action HbgMarketData::on_connect(Wss* ws) {
    size_t index = ws->get_index();
    INFRA_LOG_INFO("[hbg] [on_connect] [MarketData], connection id: {}", index);
    subscribe(index);
    return Action::NONE;
}

Action HbgMarketData::on_ping(Wss* ws, std::string_view payload) { return Action::NONE; }

Action HbgMarketData::on_ping_hbg(Wss* ws, int64_t ts) {
    // INFRA_LOG_DEBUG("[hbg] [on_ping] [MarketData], payload: {}", payload);
    std::string content = fmt::format(R"({{"pong":{}}})", ts);
    size_t index = ws->get_index();
    send_ws_request(wss_connections_[index], content, "ping");
    return Action::NONE;
}

Action HbgMarketData::on_pong(Wss* ws, std::string_view payload) {
    // INFRA_LOG_DEBUG("[hbg] [on_pong] [MarketData], payload: {}", payload);
    return Action::NONE;
}

void HbgMarketData::on_close(Wss* ws) {
    size_t index = ws->get_index();
    INFRA_LOG_WARN("[hbg] [on_close] [MarketData], msg: WebSocket connection has been closed, index: {}", index);
}

void HbgMarketData::on_error(Wss* ws, std::string_view err) {
    size_t index = ws->get_index();
    INFRA_LOG_WARN("[hbg] [on_error] [MarketData], msg: WebSocket error occurred: {}, index: {}", err, index);
}

Action HbgMarketData::on_message(Wss* ws, std::string_view msg) {
    uint64_t recv_tsc = rdtsc();
    uint64_t recv_milli = time_get_now_milli();
    std::string decode_msg = hbg_decompress_gzip(msg);
    // INFRA_LOG_DEBUG("[hbg] [gzip decompressed], size: {}, content: {}",
    //                 decompressed.size(), decompressed.substr(0, 200));
    try {
        PARSE_JSON(decode_msg, doc);
        if (doc["ch"].error() == simdjson::SUCCESS) {
            std::string channel(doc["ch"].get_string().value());
            if (channel.find(".bbo") != std::string::npos) {
                on_message_bookticker(doc["tick"], channel, recv_tsc, recv_milli);
            }
        } else if (doc["status"].error() == simdjson::SUCCESS) {
            std::string_view status = doc["status"];
            if (status == "ok") {
                INFRA_LOG_INFO("[hbg] [on_message] [market_data], recv: {}", decode_msg);
            } else {
                INFRA_LOG_WARN("[hbg] [on_message] [market_data], recv: {}", decode_msg);
            }
        } else {
            if (doc["ping"].error() == simdjson::SUCCESS) {
                on_ping_hbg(ws, doc["ping"]);
            } else {
                INFRA_LOG_WARN("[hbg] [on_message] [market_data], recv other msg: {}", decode_msg);
            }
        }

    } catch (const std::exception& ex) {
        INFRA_LOG_WARN("[hbg] [on_message] [MarketData] [exception], error: {}, msg: {}", ex.what(), decode_msg);
    }
    return Action::RECEIVE;
}

void HbgMarketData::subscribe(size_t index) {
    const auto& batch_params = stream_params_[index];
    for (const std::string& payload : batch_params) {
        INFRA_LOG_INFO("[hbg] [subscribe_orderbook], msg: connection: {}, send: {}", index, payload);
        wss_connections_[index]->send(payload);
    }
}

void HbgMarketData::on_message_bookticker(const simdjson::dom::object& data, const std::string& channel, uint64_t recv_tsc, uint64_t recv_milli) {
    std::string symbol;
    size_t start = channel.find("market.");
    if (start != std::string::npos) {
        start += 7; // 跳过 "market."
        size_t end = channel.find('.', start);
        if (end != std::string::npos) {
            symbol = channel.substr(start, end - start);
        }
    }

    Symbol pair = transfer_to_infra_pair(symbol);
    Timestamp milli = data["ts"];

    double best_ask_price = data["ask"].at(0).get_double();
    double best_bid_price = data["bid"].at(0).get_double();
    double best_ask_size = data["ask"].at(1).get_double() * g_pairs_info_cache[pair]->denomination_value;
    double best_bid_size = data["bid"].at(1).get_double() * g_pairs_info_cache[pair]->denomination_value;

    SpOrderBook orderbook = this->apply_orderbook_delta(pair, milli, best_ask_price, best_ask_size, best_bid_price, best_bid_size);
    orderbook->recv_tsc = recv_tsc;
    orderbook->recv_milli = recv_milli;
    orderbook->parsed_tsc = rdtsc();
    this->dispatch_orderbook(std::move(orderbook));
}
} // namespace infra