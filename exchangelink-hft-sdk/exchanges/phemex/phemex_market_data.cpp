#include "phemex_market_data.h"
#include <boost/asio/steady_timer.hpp>
using namespace infra::phemex;

namespace infra {
bool PhemexMarketData::initialize() {
    auto& info = g_config_map[base_config_.to_str()];
    if (info.empty()) {
        INFRA_LOG_WARN("[phemex] [initialize] [fail], msg: {} {} {} not implemented",
                       to_string(base_config_.account_type), to_string(base_config_.address_type),
                       to_string(base_config_.settle_unit));
        return false;
    }

    rest_host_ = info[REST_HOST];
    pairs_info_path_ = info[PAIRS_INFO_PATH];
    funding_fee_path_ = info[FUNDING_FEE_PATH];
    wss_infos_ = {info[WSS_PUBLIC_HOST], info[WSS_PORT], info[WSS_PUBLIC_PATH]};
    INFRA_LOG_INFO("[phemex] [initialize] [MarketData], websocket endpoint: {} {} {}", wss_infos_.host, wss_infos_.path,
                   wss_infos_.port);
    return true;
}

void PhemexMarketData::shutdown() { unsubscribe_orderbook(); }

bool PhemexMarketData::subscribe_orderbook(const Symbols& symbols, unsigned int depth, OrderbookCallback cb) {
    if (depth != 1 && depth != 5 && depth != 10 && depth != 30) {
        INFRA_LOG_WARN("[phemex] [subscribe_orderbook] [fail] msg: unsupported depth level {}", depth);
        return false;
    }
    this->orderbook_handler_ = std::move(cb);
    const Symbols& sub_symbols = (!symbols.empty()) ? symbols : g_all_symbols;
    size_t total = sub_symbols.size();
    std::vector<std::string> params;
    for (size_t i = 1; i <= total; i++) {
        std::string pair = transfer_from_infra_pair(sub_symbols[i - 1]);
        std::string param = fmt::format(R"("{}",false,{})", pair, std::to_string(depth));
        std::string payload = R"({"id":)" + std::to_string(get_id()) +
                              R"(,"method":"orderbook_p.subscribe","params":[)" + param + R"(]})";
        params.push_back(payload);
        if (i % MAX_PAIRS_PER_WS_CONNECTION == 0 || i == total) {
            stream_params_.push_back(std::move(params));
        }
    }
    size_t conn_num = stream_params_.size();
    INFRA_LOG_INFO("[phemex] [subscribe_orderbook], establishing {} WebSocket connections", conn_num);
    for (size_t i = 0; i < conn_num; i++) {
        auto client = std::make_shared<WebSocketClient>(ioc_, ssl_ctx_, *this);
        wss_connections_.push_back(client);
        auto& conn = wss_connections_.back();
        conn->set_user_data(i); // index of wss_connections_
        conn->resolve_connect(wss_infos_.host, wss_infos_.port, wss_infos_.path);
    }
    return true;
}

void PhemexMarketData::unsubscribe_orderbook() {
    this->orderbook_handler_ = nullptr;
    for (auto& conn : wss_connections_) {
        if (conn->is_socket_open()) {
            conn->close();
        }
    }
    stream_params_.clear();
    wss_connections_.clear();
    INFRA_LOG_INFO("[phemex] [unsubscribe_orderbook] [success]");
}

void PhemexMarketData::fetch_pairs_info(ExPairInfoCallback cb) {
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
                if (doc["code"].error() != simdjson::SUCCESS || doc["code"].get_int64() != SUCCESS_CODE) {
                    break;
                }
                Currency currency = to_string(base_config_.settle_unit);
                parse_pairs_info(doc, currency);
                INFRA_LOG_INFO("[phemex] [fetch_pairs_info] [success], size: {}", g_pairs_info_cache.size());
                cb(Errno::Ok, g_pairs_info_cache);
                return;
            } catch (const std::exception& ex) {
                INFRA_LOG_WARN("[phemex] [fetch_pairs_info] [exception], msg: {}", ex.what());
            }
        } while (0);
        INFRA_LOG_WARN("[phemex] [fetch_pairs_info] [fail], recv: {}", response);
        cb(extract_error_code(response), {});
    });
}

void PhemexMarketData::fetch_funding_fee(const Symbol& symbol, FundingFeeCallback cb) {
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
                if (!doc["error"].is_null()) {
                    break;
                }
                INFRA_LOG_INFO("[phemex] [fetch_funding_fee] [success], recv: {}", response);
                auto fee = parse_funding_fee(doc);
                cb(Errno::Ok, fee);
                return;
            } catch (const std::exception& ex) {
                INFRA_LOG_WARN("[phemex] [fetch_funding_fee] [exception], msg: {}", ex.what());
            }
        } while (0);
        INFRA_LOG_WARN("[phemex] [fetch_funding_fee] [fail], recv: {}", response);
        cb(extract_error_code(response), nullptr);
    });
}

Action PhemexMarketData::on_connect(Wss* ws) {
    size_t index = ws->get_index();
    INFRA_LOG_INFO("[phemex] [on_connect] [MarketData], msg: WebSocket connection established, index: {}", index);
    if (index < wss_connections_.size()) {
        auto client = wss_connections_[index];
        keep_ws_connection_alive(*client);
        subscribe(index);
    } else {
        INFRA_LOG_WARN("[phemex] [on_connect] [MarketData], msg: invalid connection index: {}", index);
    }
    return Action::NONE;
}

Action PhemexMarketData::on_ping(Wss* ws, std::string_view payload) {
    // INFRA_LOG_DEBUG("[phemex] [on_ping] [MarketData], payload: {}", payload);
    ws->pong(std::string(payload));
    return Action::NONE;
}

Action PhemexMarketData::on_pong(Wss* ws, std::string_view payload) {
    // INFRA_LOG_DEBUG("[phemex] [on_pong] [MarketData], payload: {}", payload);
    return Action::NONE;
}

void PhemexMarketData::on_close(Wss* ws) {
    size_t index = ws->get_index();
    INFRA_LOG_WARN("[phemex] [on_close] [MarketData], msg: WebSocket connection has been closed, index: {}", index);
}

void PhemexMarketData::on_error(Wss* ws, std::string_view err) {
    size_t index = ws->get_index();
    INFRA_LOG_WARN("[phemex] [on_error] [MarketData], msg: WebSocket error occurred: {}, index: {}", err, index);
}

Action PhemexMarketData::on_message(Wss* ws, std::string_view msg) {
    // INFRA_LOG_INFO("[phemex] [on_message] [MarketData], msg: {}", msg);
    uint64_t recv_tsc = rdtsc();
    uint64_t recv_milli = time_get_now_milli();
    try {
        PARSE_JSON(msg, doc);
        if (doc["id"].error() != simdjson::SUCCESS) {
            on_message_bookticker(doc, recv_tsc, recv_milli);
        } else if (doc["result"].error() == simdjson::SUCCESS) {
            if (doc["result"].is_object())
            {
                std::string_view status = doc["result"]["status"];
                if (status != "success") {
                    INFRA_LOG_WARN("[phemex] [on_message] [subscribe_orderbook] [fail], msg: {}", msg);
                } else {
                    INFRA_LOG_INFO("[phemex] [on_message] [subscribe_orderbook] [success], msg: {}", msg);
                }
            } else if (doc["result"].is_string()) {
                std::string_view ret = doc["result"];
                if (ret != "pong") {
                    INFRA_LOG_WARN("[phemex] [on_message], unexcepted msg: {}", msg);
                }
            }
        } else if (doc["index_market24h"].error() == simdjson::SUCCESS) {
            // ignore market24h msg
        } else {
            INFRA_LOG_WARN("[phemex] [on_message], unexcepted msg: {}", msg);
        }
    } catch (const std::exception& ex) {
        INFRA_LOG_WARN("[phemex] [on_message] [exception], error: {}, msg: {}", ex.what(), msg);
    }
    return Action::RECEIVE;
}

void PhemexMarketData::subscribe(size_t index) {
    size_t delay_s = index + 2; // NOTE：延迟订阅，提高成功率
    auto timer = std::make_shared<boost::asio::steady_timer>(ioc_, std::chrono::seconds(delay_s));
    timer->async_wait([this, index, timer](const boost::system::error_code& ec) {
    for (std::string payload : stream_params_[index]) {
        INFRA_LOG_INFO("[phemex] [subscribe_orderbook], connection {}, send: {}", index, payload);
        wss_connections_[index]->send(std::move(payload));
    }
    });
}

void PhemexMarketData::on_message_bookticker(const simdjson::dom::object& data, uint64_t recv_tsc, uint64_t recv_milli) {
    simdjson::dom::object orderbook_obj = data["orderbook_p"];
    std::string_view symbol = data["symbol"];
    int64_t milli_text = data["timestamp"].get_int64();
    std::string pair(transfer_to_infra_pair(symbol));
    double denomination = get_denomination_value(pair);
    Timestamp milli = milli_text / 1000000;
    
    double best_ask_price = 0.0;
    double best_ask_size = 0.0;
    double best_bid_price = 0.0;
    double best_bid_size = 0.0;

    auto asks = orderbook_obj["asks"].get_array();
    for (auto&& items : asks) {
        auto it = items.begin();
        best_ask_price = str_to_float(std::string_view(*it));
        ++it;
        best_ask_size = str_to_float(std::string_view(*it)) * denomination;
        break;
    }

    auto bids = orderbook_obj["bids"].get_array();
    for (auto&& items : bids) {
        auto it = items.begin();
        best_bid_price = str_to_float(std::string_view(*it));
        ++it;
        best_bid_size = str_to_float(std::string_view(*it)) * denomination;
        break;
    }

    SpOrderBook orderbook = this->apply_orderbook_delta( pair, milli, best_ask_price, best_ask_size, best_bid_price, best_bid_size);
    orderbook->recv_tsc = recv_tsc;
    orderbook->recv_milli = recv_milli;
    orderbook->parsed_tsc = rdtsc();
    this->dispatch_orderbook(std::move(orderbook));
}
} // namespace infra