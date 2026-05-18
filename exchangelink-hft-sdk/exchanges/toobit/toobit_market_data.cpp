#include "toobit_market_data.h"
using namespace infra::toobit;

namespace infra {
bool ToobitMarketData::initialize() {
    auto& info = g_config_map[base_config_.to_str()];
    if (info.empty()) {
        INFRA_LOG_WARN("[toobit] [initialize] [fail], msg: {} {} {} not implemented",
                       to_string(base_config_.account_type), to_string(base_config_.address_type),
                       to_string(base_config_.settle_unit));
        return false;
    }

    rest_host_ = info[REST_HOST];
    pairs_info_path_ = info[PAIRS_INFO_PATH];
    funding_fee_path_ = info[FUNDING_FEE_PATH];
    his_funding_fee_path_ = info[HISTORY_FUNDING_FEE_PATH];
    place_order_path_ = info[PLACE_ORDER_PATH_PATH];

    wss_infos_ = {info[WSS_PUBLIC_HOST], info[WSS_PORT], info[WSS_PUBLIC_PATH]};
    INFRA_LOG_INFO("[toobit] [initialize] [MarketData], websocket endpoint: {} {} {}", wss_infos_.host, wss_infos_.path,
                   wss_infos_.port);
    return true;
}

void ToobitMarketData::shutdown() { unsubscribe_orderbook(); }

bool ToobitMarketData::subscribe_orderbook(const Symbols& symbols, unsigned int depth, OrderbookCallback cb) {
    if (depth != 1) {
        INFRA_LOG_WARN("[toobit] [subscribe_orderbook] [fail], msg: unsupported depth level {}", depth);
        return false;
    }

    this->orderbook_handler_ = std::move(cb);
    const Symbols& sub_symbols = (!symbols.empty()) ? symbols : g_all_symbols;
    stream_params_.clear();
    std::string param{};
    size_t total = sub_symbols.size();
    for (size_t i = 1; i <= total; i++) {
        std::string pair = transfer_from_infra_pair(sub_symbols[i - 1]);
        param.append(pair).append(",");
        if (i % MAX_PAIRS_PER_WS_CONNECTION == 0 || i == total) {
            param.pop_back(); // remove last ','
            stream_params_.push_back(std::move(param));
            param.clear();
        }
    }

    INFRA_LOG_INFO("[toobit] [subscribe_orderbook], msg: establishing {} WebSocket connections", stream_params_.size());
    for (size_t i = 0; i < stream_params_.size(); i++) {
        auto client = std::make_shared<WebSocketClient>(ioc_, ssl_ctx_, *this);
        wss_connections_.push_back(client);
        auto& conn = wss_connections_.back();
        conn->set_user_data(i); // index of wss_connections_
        conn->resolve_connect(wss_infos_.host, wss_infos_.port, wss_infos_.path);
    }
    return true;
}

void ToobitMarketData::unsubscribe_orderbook() {
    this->orderbook_handler_ = nullptr;
    for (auto& conn : wss_connections_) {
        if (conn->is_socket_open()) {
            conn->close();
        }
    }
    stream_params_.clear();
    wss_connections_.clear();
    INFRA_LOG_INFO("[toobit] [unsubscribe_orderbook] [success]");
}

void ToobitMarketData::fetch_pairs_info(ExPairInfoCallback cb) {
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
                Currency currency = to_string(base_config_.settle_unit);
                parse_pairs_info(doc, currency);
                INFRA_LOG_INFO("[toobit] [fetch_pairs_info] [success], size: {}", g_pairs_info_cache.size());
                cb(Errno::Ok, g_pairs_info_cache);
                return;
            } catch (const std::exception& ex) {
                INFRA_LOG_WARN("[toobit] [fetch_pairs_info] [exception], msg: {}", ex.what());
            }
        } while (0);
        INFRA_LOG_WARN("[toobit] [fetch_pairs_info] [fail], recv: {}", response);
        cb(extract_error_code(response), {});
    });
}

void ToobitMarketData::fetch_funding_fee(const Symbol& symbol, FundingFeeCallback cb) {
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
                INFRA_LOG_INFO("[toobit] [fetch_funding_fee] [success], recv: {}", response);
                auto fee = parse_funding_fee(doc);
                cb(Errno::Ok, fee);
                return;
            } catch (const std::exception& ex) {
                INFRA_LOG_WARN("[toobit] [fetch_funding_fee] [exception], msg: {}", ex.what());
            }
        } while (0);
        INFRA_LOG_WARN("[toobit] [fetch_funding_fee] [fail], recv: {}", response);
        cb(extract_error_code(response), nullptr);
    });
}

Action ToobitMarketData::on_connect(Wss* ws) {
    size_t index = ws->get_user_data();
    INFRA_LOG_INFO("[toobit] [on_connect] [MarketData], msg: WebSocket connection established, index: {}", index);
    if (LIKELY(index < wss_connections_.size())) {
        keep_ws_connection_alive(index);
        subscribe(index);
    } else {
        INFRA_LOG_WARN("[toobit] [on_connect] [MarketData], msg: invalid connection index: {}", index);
    }
    return Action::NONE;
}

Action ToobitMarketData::on_ping(Wss* ws, std::string_view payload) {
    // INFRA_LOG_INFO("[toobit] [on_ping] [MarketData], payload: {}", payload);
    ws->pong(std::string(payload));
    return Action::NONE;
}

Action ToobitMarketData::on_pong(Wss* ws, std::string_view payload) {
    // INFRA_LOG_INFO("[toobit] [on_pong] [MarketData], payload: {}", payload);
    return Action::NONE;
}

void ToobitMarketData::on_close(Wss* ws) {
    size_t index = ws->get_user_data();
    INFRA_LOG_WARN("[toobit] [on_close] [MarketData], msg: WebSocket connection has been closed, index: {}", index);
}

void ToobitMarketData::on_error(Wss* ws, std::string_view err) {
    size_t index = ws->get_user_data();
    INFRA_LOG_WARN("[toobit] [on_error] [MarketData], msg: WebSocket error occurred: {}, index: {}", err, index);
}

Action ToobitMarketData::on_message(Wss* ws, std::string_view msg) {
    // INFRA_LOG_INFO("[toobit] [on_message] [MarketData], msg: {}", msg);
    try {
        PARSE_JSON(msg, doc);
        if (doc["data"].error() == simdjson::SUCCESS) {
            on_message_bookticker(doc);
        } else if (doc["pong"].error() == simdjson::SUCCESS) {
            // ignore
        } else {
            INFRA_LOG_INFO("[toobit] [on_message] [MarketData], msg: {}", msg);
        }
    } catch (const std::exception& ex) {
        INFRA_LOG_WARN("[toobit] [on_message] [exception], error: {}, msg: {}", ex.what(), msg);
    }
    return Action::RECEIVE;
}

void ToobitMarketData::keep_ws_connection_alive(size_t index) {
    std::string msg = fmt::format(R"({{"ping":{}}})", time_get_now_milli());
    wss_connections_[index]->start_ping_pong(msg, 10);
}

void ToobitMarketData::subscribe(size_t index) {
    std::string tmp = stream_params_[index];
    std::string payload =
        fmt::format(R"({{"symbol":"{}","topic":"depth","event":"sub","params":{{"binary":false}}}})", tmp);
    INFRA_LOG_INFO("[toobit] [subscribe_orderbook], connection {}, send: {}", index, payload);
    wss_connections_[index]->send(std::move(payload));
}

void ToobitMarketData::on_message_bookticker(const simdjson::dom::element& doc) {
    simdjson::dom::array array = doc["data"];
    for (auto item : array) {
        std::string_view symbol_text = item["s"];
        Symbol pair = transfer_to_infra_pair(symbol_text);
        bfloat denomination = get_denomination_value(pair);
        Timestamp milli = item["t"];

        std::list<Level> asks, bids;
        conj_orderbook_sides(item["a"], asks, denomination);
        conj_orderbook_sides(item["b"], bids, denomination);

        SpOrderBook orderbook = this->apply_orderbook_delta(true, pair, milli, asks, bids);
        this->dispatch_orderbook(std::move(orderbook));
    }
}
} // namespace infra