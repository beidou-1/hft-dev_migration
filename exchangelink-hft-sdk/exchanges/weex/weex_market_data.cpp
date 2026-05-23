#include "weex_market_data.h"
#include <boost/asio/steady_timer.hpp>
using namespace infra::weex;

namespace infra {
bool WeexMarketData::initialize() {
    auto& info = g_config_map[base_config_.to_str()];
    if (info.empty()) {
        INFRA_LOG_WARN("[weex] [initialize] [fail], msg: {} {} {} not implemented",
                       to_string(base_config_.account_type), to_string(base_config_.address_type),
                       to_string(base_config_.settle_unit));
        return false;
    }

    rest_host_ = info[REST_HOST];
    pairs_info_path_ = info[PAIRS_INFO_PATH];
    funding_fee_path_ = info[FUNDING_FEE_PATH];

    wss_infos_ = {info[WSS_PUBLIC_HOST], info[WSS_PORT], info[WSS_PUBLIC_PATH]};
    INFRA_LOG_INFO("[weex] [initialize] [MarketData], websocket endpoint: {} {} {}", wss_infos_.host, wss_infos_.path,
                   wss_infos_.port);
    return true;
}

void WeexMarketData::shutdown() { unsubscribe_orderbook(); }

bool WeexMarketData::subscribe_orderbook(const Symbols& symbols, unsigned int depth, OrderbookCallback cb) {
    if (depth != 1 && depth != 15) {
        INFRA_LOG_WARN("[weex] [subscribe_orderbook] [fail], msg: unsupported depth level {}", depth);
        return false;
    }

    this->orderbook_handler_ = std::move(cb);
    const Symbols& sub_symbols = (!symbols.empty()) ? symbols : g_all_symbols;
    stream_params_.clear();

    std::ostringstream oss{};
    size_t total = sub_symbols.size();
    for (size_t i = 1; i <= total; i++) {
        std::string pair = transfer_from_infra_pair(sub_symbols[i - 1]);
        oss << "\"" << pair << "@depth15\",";
        if (i % MAX_PAIRS_PER_WS_CONNECTION == 0 || i == total) {
            std::string params = oss.str();
            params.pop_back(); // remove last ','
            stream_params_.push_back(std::move(params));
            oss.str("");
        }
    }

    INFRA_LOG_INFO("[weex] [subscribe_orderbook], msg: establishing {} WebSocket connections", stream_params_.size());
    for (size_t i = 0; i < stream_params_.size(); i++) {
        auto client = std::make_shared<WebSocketClient>(ioc_, ssl_ctx_, *this);
        wss_connections_.push_back(client);
        auto& conn = wss_connections_.back();
        conn->set_user_data(i); // index of wss_connections_
        conn->resolve_connect(wss_infos_.host, wss_infos_.port, wss_infos_.path);
    }
    return true;
}

void WeexMarketData::unsubscribe_orderbook() {
    this->orderbook_handler_ = nullptr;
    for (auto& conn : wss_connections_) {
        if (conn->is_socket_open())
            conn->close();
    }
    stream_params_.clear();
    wss_connections_.clear();
    INFRA_LOG_INFO("[weex] [unsubscribe_orderbook] [success]");
}

void WeexMarketData::fetch_pairs_info(ExPairInfoCallback cb) {
    if (pairs_info_path_.empty()) {
        cb(Errno::NotImplemented, {});
        return;
    }

    auto req = get_request_body(rest_host_, pairs_info_path_);
    rest_.send(req, [this, cb](HttpResponseBody& res) {
        std::string response = boost::beast::buffers_to_string(res.body().data());
        do {
            if (res.result() != HTTP_STATUS_OK)
                break;
            try {
                PARSE_JSON(response, doc);
                if (doc["code"].error() == simdjson::SUCCESS && doc["code"].get_string() != SUCCESS_CODE)
                    break;
                Currency currency = to_string(base_config_.settle_unit);
                parse_pairs_info(doc, currency);
                INFRA_LOG_INFO("[weex] [fetch_pairs_info] [success], size: {}", g_pairs_info_cache.size());
                cb(Errno::Ok, g_pairs_info_cache);
                return;
            } catch (const std::exception& ex) {
                INFRA_LOG_WARN("[weex] [fetch_pairs_info] [exception], msg: {}", ex.what());
            }
        } while (0);
        INFRA_LOG_WARN("[weex] [fetch_pairs_info] [fail], recv: {}", response);
        cb(extract_error_code(response), {});
    });
}

void WeexMarketData::fetch_funding_fee(const Symbol& symbol, FundingFeeCallback cb) {
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
            if (res.result() != HTTP_STATUS_OK)
                break;
            try {
                PARSE_JSON(response, doc);
                if (doc["code"].error() == simdjson::SUCCESS && doc["code"].get_string() != SUCCESS_CODE)
                    break;
                INFRA_LOG_INFO("[weex] [fetch_funding_fee] [success], recv: {}", response);
                cb(Errno::Ok, parse_funding_fee(doc));
                return;
            } catch (const std::exception& ex) {
                INFRA_LOG_WARN("[weex] [fetch_funding_fee] [exception], msg: {}", ex.what());
            }
        } while (0);
        INFRA_LOG_WARN("[weex] [fetch_funding_fee] [fail], recv: {}", response);
        cb(extract_error_code(response), nullptr);
    });
}

Action WeexMarketData::on_connect(Wss* ws) {
    size_t index = ws->get_index();
    INFRA_LOG_INFO("[weex] [on_connect] [MarketData], index: {}", index);
    if (index < wss_connections_.size()) {
        subscribe(index);
    }
    return Action::NONE;
}

Action WeexMarketData::on_ping(Wss* ws, std::string_view payload) {
    ws->pong(std::string(payload));
    return Action::NONE;
}

Action WeexMarketData::on_pong(Wss* ws, std::string_view payload) { return Action::NONE; }

void WeexMarketData::on_close(Wss* ws) {
    INFRA_LOG_WARN("[weex] [on_close] [MarketData], index: {}", ws->get_index());
}

void WeexMarketData::on_error(Wss* ws, std::string_view err) {
    INFRA_LOG_WARN("[weex] [on_error] [MarketData], err: {}, index: {}", err, ws->get_index());
}

Action WeexMarketData::on_message(Wss* ws, std::string_view msg) {
    // INFRA_LOG_INFO("[weex] [on_message] [MarketData], msg: {}", msg);
    uint64_t recv_tsc = rdtsc();
    uint64_t recv_milli = time_get_now_milli();
    try {
        PARSE_JSON(msg, doc);
        if (doc["e"].error() == simdjson::SUCCESS) {
            std::string_view event = doc["e"];
            if (event == "depthSnapshot" || event == "depth") {
                on_message_orderbook(doc, recv_tsc, recv_milli);
            } else {
                INFRA_LOG_WARN("[weex] [on_message] [MarketData] unexpected msg: {}", msg);
            }
        } else if (doc["event"].error() == simdjson::SUCCESS) {
            std::string_view event = doc["event"];
            if (event == "connected") {
                INFRA_LOG_INFO("[weex] [on_message] [MarketData] recv: {}", msg);
            } else if (event == "ping") {
                size_t index = ws->get_index();
                std::string pong_msg = fmt::format(R"({{"method":"PONG","id":{}}})", index);
                wss_connections_[index]->send(std::move(pong_msg));
            } else {
                INFRA_LOG_WARN("[weex] [on_message] [MarketData] unexpected msg: {}", msg);
            }
        } else if (doc["result"].error() == simdjson::SUCCESS) {
            bool result = doc["result"];
            if (result) {
                INFRA_LOG_INFO("[weex] [on_message] [MarketData] recv: {}", msg);
            } else {
                INFRA_LOG_WARN("[weex] [on_message] [MarketData] recv: {}", msg);
            }
        } else {
            INFRA_LOG_WARN("[weex] [on_message] [MarketData] unexpected msg: {}", msg);
        }
    } catch (const std::exception& ex) {
        INFRA_LOG_WARN("[weex] [on_message] [MarketData] [exception], error: {}, msg: {}", ex.what(), msg);
    }
    return Action::RECEIVE;
}

void WeexMarketData::subscribe(size_t index) {
    std::string payload =
        fmt::format(R"({{"id":{},"method":"SUBSCRIBE","params":[{}]}})", index, stream_params_[index]);
    INFRA_LOG_INFO("[weex] [subscribe_orderbook], connection {}, send: {}", index, payload);
    wss_connections_[index]->send(std::move(payload));
}

void WeexMarketData::on_message_orderbook(const simdjson::dom::element& doc, uint64_t recv_tsc, uint64_t recv_milli) {
    std::string_view symbol = doc["s"];
    std::string_view depthType = doc["d"];
    bool is_full = (depthType == "CHANGED") ? false : true;
    int64_t update_id = doc["u"];
    int64_t milli = doc["E"];

    double best_ask_price = 0.0;
    double best_ask_size = 0.0;
    double best_bid_price = 0.0;
    double best_bid_size = 0.0;
    
    std::list<Level> asks, bids;
    conj_orderbook_sides(doc["a"], asks);
    conj_orderbook_sides(doc["b"], bids);

    Symbol pair = transfer_to_infra_pair(symbol);
    SpOrderBook orderbook = this->apply_orderbook_delta( pair, milli, asks, bids, , best_ask_price, best_ask_size, best_bid_price, best_bid_size);
    orderbook->recv_tsc = recv_tsc;
    orderbook->recv_milli = recv_milli;
    orderbook->parsed_tsc = rdtsc();
    this->dispatch_orderbook(std::move(orderbook));
}
} // namespace infra
