#include "okex_market_data.h"
#include "okex_utils.h"
using namespace infra::okex;
using namespace boost::beast;

namespace infra {
bool OkxMarketData::initialize() {
    auto& info = g_config_map[base_config_.to_str()];
    if (info.empty()) {
        INFRA_LOG_WARN("[okex] [initialize] [fail], msg: {} {} {} not implemented",
                       to_string(base_config_.account_type), to_string(base_config_.address_type),
                       to_string(base_config_.settle_unit));
        return false;
    }

    if (account_secret_.api_key.empty() || account_secret_.api_secret.empty() || account_secret_.api_phrase.empty()) {
        INFRA_LOG_WARN("[okex] [initialize] [fail], msg: AccountSecret filed is empty");
        return false;
    }

    rest_host_ = info[REST_HOST];
    pairs_info_path_ = info[PAIRS_INFO_PATH];
    funding_fee_path_ = info[FUNDING_FEE_PATH];
    wss_infos_ = {info[WSS_PUBLIC_HOST], info[WSS_PORT], info[WSS_PUBLIC_PATH]};
    INFRA_LOG_INFO("[okex] [initialize] [MarketData], websocket endpoint: {} {} {}", wss_infos_.host, wss_infos_.path,
                   wss_infos_.port);
    return true;
}

void OkxMarketData::shutdown() { unsubscribe_orderbook(); }

bool OkxMarketData::subscribe_orderbook(const Symbols& symbols, unsigned int depth, OrderbookCallback cb) {
    if (depth != 1 && depth != 5) {
        INFRA_LOG_WARN("[okex] [subscribe_orderbook] [fail], msg: unsupported depth level {}", depth);
        return false;
    }
    std::string sub_event{};
    if (depth == 1) {
        sub_event = "bbo-tbt";
    } else if (depth == 5) {
        sub_event = "books5";
    }
    orderbook_handler_ = std::move(cb);
    const Symbols& sub_symbols = (!symbols.empty()) ? symbols : g_all_symbols;
    size_t total = sub_symbols.size();
    std::ostringstream oss{};
    for (size_t i = 1; i <= total; i++) {
        std::string pair = transfer_from_infra_pair(sub_symbols[i - 1]);
        oss << "{\"channel\":\"" << sub_event << "\",\"instId\":\"" << pair << "\"},";
        if (i % MAX_PAIRS_PER_WS_CONNECTION == 0 || i == total) {
            std::string params = oss.str();
            params.pop_back(); // remove last ','
            stream_params_.push_back(std::move(params));
            oss.str("");
        }
    }
    INFRA_LOG_INFO("[okex] [subscribe_orderbook], establishing {} WebSocket connections", stream_params_.size());
    for (size_t i = 0; i < stream_params_.size(); i++) {
        auto client = std::make_shared<WebSocketClient>(ioc_, ssl_ctx_, *this);
        wss_connections_.push_back(client);
        auto& conn = wss_connections_.back();
        conn->set_user_data(i); // index of wss_connections_
        conn->resolve_connect(wss_infos_.host, wss_infos_.port, wss_infos_.path);
    }
    return true;
}

void OkxMarketData::unsubscribe_orderbook() {
    this->orderbook_handler_ = nullptr;
    for (auto& conn : wss_connections_) {
        if (conn->is_socket_open()) {
            conn->close();
        }
    }
    stream_params_.clear();
    wss_connections_.clear();
    INFRA_LOG_INFO("[okex] [unsubscribe_orderbook] [success]");
}

void OkxMarketData::fetch_pairs_info(ExPairInfoCallback cb) {
    if (pairs_info_path_.empty()) {
        cb(Errno::NotImplemented, {});
        return;
    }

    auto req = get_request_body(rest_host_, pairs_info_path_, "instType=SWAP");
    rest_.send(req, [this, cb](HttpResponseBody& res) {
        std::string response = boost::beast::buffers_to_string(res.body().data());
        if (res.result() == http::status::ok) {
            Currency currency = to_string(base_config_.settle_unit);
            parse_pairs_info(response, currency);
            INFRA_LOG_INFO("[okex] [fetch_pairs_info] [success], size: {}", g_pairs_info_cache.size());
            cb(Errno::Ok, g_pairs_info_cache);
        } else {
            INFRA_LOG_WARN("[okex] [fetch_pairs_info] [fail], recv: {}", response);
            cb(extract_error_msg(response), {});
        }
    });
}

void OkxMarketData::fetch_funding_fee(const Symbol& symbol, FundingFeeCallback cb) {
    if (funding_fee_path_.empty()) {
        cb(Errno::NotImplemented, nullptr);
        return;
    }

    if (symbol.empty()) {
        cb(Errno::InvalidParams, nullptr);
        return;
    }
    std::string query = "instId=" + transfer_from_infra_pair(symbol);
    auto req = get_request_body(rest_host_, funding_fee_path_, query);
    rest_.send(req, [this, cb](HttpResponseBody& res) {
        std::string response = boost::beast::buffers_to_string(res.body().data());
        if (res.result() == http::status::ok) {
            auto fee = parse_funding_fee(response);
            cb(Errno::Ok, fee);
        } else {
            INFRA_LOG_WARN("[okex] [fetch_funding_fee] [fail], response: {}", response);
            cb(extract_error_msg(response), nullptr);
        }
    });
}

Action OkxMarketData::on_connect(Wss* ws) {
    size_t index = ws->get_index();
    INFRA_LOG_INFO("[okex] [on_connect] [MarketData], msg: WebSocket connection established, index: {}", index);
    if (index < wss_connections_.size()) {
        auto client = wss_connections_[index];
        keep_ws_connection_alive(*client);
        subscribe(index);
    } else {
        INFRA_LOG_WARN("[okex] [on_connect] [MarketData], msg: invalid connection index: {}", index);
    }
    return Action::NONE;
}

Action OkxMarketData::on_ping(Wss* ws, std::string_view payload) {
    // INFRA_LOG_DEBUG("[okex] [on_ping] [MarketData], payload: {}", payload);
    ws->pong(std::string(payload));
    return Action::NONE;
}

Action OkxMarketData::on_pong(Wss* ws, std::string_view payload) {
    // INFRA_LOG_DEBUG("[okex] [on_pong] [MarketData], recv pong payload: {}", payload);
    return Action::NONE;
}

void OkxMarketData::on_close(Wss* ws) {
    INFRA_LOG_WARN("[okex] [on_close] [fail], msg: WebSocket connection has been closed");
}

void OkxMarketData::on_error(Wss* ws, std::string_view err) {
    INFRA_LOG_WARN("[okex] [on_error] [fail], msg: WebSocket error occurred: {}", err);
}

Action OkxMarketData::on_message(Wss* ws, std::string_view msg) {
    // INFRA_LOG_INFO("[okex] [on_message] [success], msg: {}", msg);
    uint64_t recv_tsc = rdtsc();
    uint64_t recv_milli = time_get_now_milli();
    if (msg == "pong") {
        return Action::RECEIVE;
    }
    try {
        PARSE_JSON(msg, doc);
        auto data_elem = doc["data"];
        if (data_elem.error() == simdjson::SUCCESS) {
            std::string_view symbol = doc["arg"]["instId"];
            for (auto item : data_elem.get_array()) {
                on_message_depthall(item, symbol, recv_tsc, recv_milli);
            }
        } else if (doc["event"].error() == simdjson::SUCCESS) {
            std::string_view event = doc["event"];
            if (event == "login" || event == "subscribe") {
                INFRA_LOG_INFO("[okex] [on_message], recv: {}", msg);
            } else if (event == "channel-conn-count") {
                INFRA_LOG_INFO("[okex] [on_message], channel-conn-count: {}", msg);
            } else {
                INFRA_LOG_WARN("[okex] [on_message], unexcepted event: {}", event);
            }
        }
    } catch (const std::exception& ex) {
        INFRA_LOG_WARN("[okex] [on_message] [fail], parse error: {}, content: {}", ex.what(), msg);
    }
    return Action::RECEIVE;
}

void OkxMarketData::subscribe(size_t index) {
    std::string payload =
        R"({"id":")" + std::to_string(index) + R"(","op":"subscribe","args":[)" + stream_params_[index] + R"(]})";
    INFRA_LOG_INFO("[okex] [subscribe], connection {}, send: {}", index, payload);
    wss_connections_[index]->send(std::move(payload));
}

void OkxMarketData::on_message_depthall(const simdjson::dom::object& data, std::string_view symbol, uint64_t recv_tsc, uint64_t recv_milli) {
    std::string pair = transfer_to_infra_pair(symbol);
    double denomination = get_denomination_value(pair);
    std::string_view milli = data["ts"];

    double best_ask_price;
    double best_ask_size;
    double best_bid_price;
    double best_bid_size;

    auto asks = data["asks"].get_array();
    for (auto&& items : asks) {
        auto it = items.begin();
        best_ask_price = str_to_float(std::string_view(*it));
        ++it;
        best_ask_size = str_to_float(std::string_view(*it)) * denomination;
        break;
    }

    auto bids = data["bids"].get_array();
    for (auto&& items : bids) {
        auto it = items.begin();
        best_bid_price = str_to_float(std::string_view(*it));
        ++it;
        best_bid_size = str_to_float(std::string_view(*it)) * denomination;
        break;
    }

    SpOrderBook orderbook =
        this->apply_orderbook_delta(pair, std::stoll(std::string(milli)), best_ask_price, best_ask_size, best_bid_price, best_bid_size);
    orderbook->recv_tsc = recv_tsc;
    orderbook->recv_milli = recv_milli;
    orderbook->parsed_tsc = rdtsc();
    this->dispatch_orderbook(std::move(orderbook));
}
} // namespace infra
