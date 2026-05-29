#include "edgex_market_data.h"
#include <boost/asio/steady_timer.hpp>
using namespace infra::edgex;

namespace infra {
bool EdgexMarketData::initialize() {
    auto& info = g_config_map[base_config_.to_str()];
    if (info.empty()) {
        INFRA_LOG_WARN("[edgex] [initialize] [fail], msg: {} {} {} not implemented",
                       to_string(base_config_.account_type), to_string(base_config_.address_type),
                       to_string(base_config_.settle_unit));
        return false;
    }

    rest_host_ = info[REST_HOST];
    pairs_info_path_ = info[PAIRS_INFO_PATH];
    funding_fee_path_ = info[FUNDING_FEE_PATH];
    wss_infos_ = {info[WSS_PUBLIC_HOST], info[WSS_PORT], info[WSS_PUBLIC_PATH]};
    INFRA_LOG_INFO("[edgex] [initialize] [MarketData], websocket endpoint: {} {} {}", wss_infos_.host, wss_infos_.path,
                   wss_infos_.port);
    // NOTE: subscribe_orderbook依赖pairs_info_cache，因此需要先获取pairs_info
    fetch_pairs_info_sync();
    return !g_pairs_info_cache.empty();
}

void EdgexMarketData::shutdown() { unsubscribe_orderbook(); }

bool EdgexMarketData::subscribe_orderbook(const Symbols& symbols, unsigned int depth, OrderbookCallback cb) {
    this->orderbook_handler_ = std::move(cb);
    const Symbols& sub_symbols = (!symbols.empty()) ? symbols : g_all_symbols;
    size_t total = sub_symbols.size();
    std::vector<std::string> params;
    for (size_t i = 1; i <= total; i++) {
        const Symbol& pair = sub_symbols[i - 1];
        std::string coinId;
        if (!get_contract_id(pair, coinId)) {
            INFRA_LOG_WARN("[edgex] [subscribe_orderbook] [fail], msg: get contract id failed for pair {}", pair);
            continue;
        }
        std::string payload = R"({"type":"subscribe","channel":"depth.)" + coinId + R"(.15"})";
        params.push_back(payload);
        if (i % MAX_PAIRS_PER_WS_CONNECTION == 0 || i == total) {
            stream_params_.push_back(std::move(params));
        }
    }
    size_t conn_num = stream_params_.size();
    INFRA_LOG_INFO("[edgex] [subscribe_orderbook], establishing {} WebSocket connections", conn_num);
    for (size_t i = 0; i < conn_num; i++) {
        auto client = std::make_shared<WebSocketClient>(ioc_, ssl_ctx_, *this);
        wss_connections_.push_back(client);
        auto& conn = wss_connections_.back();
        conn->set_user_data(i); // index of wss_connections_
        conn->resolve_connect(wss_infos_.host, wss_infos_.port, wss_infos_.path);
    }
    INFRA_LOG_INFO("[edgex] [subscribe_orderbook], WebSocket connections end");
    return true;
}

void EdgexMarketData::unsubscribe_orderbook() {
    this->orderbook_handler_ = nullptr;
    for (auto& conn : wss_connections_) {
        if (conn->is_socket_open()) {
            conn->close();
        }
    }
    stream_params_.clear();
    wss_connections_.clear();
    INFRA_LOG_INFO("[edgex] [unsubscribe_orderbook] [success]");
}

void EdgexMarketData::fetch_pairs_info(ExPairInfoCallback cb) {
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
                if (doc["code"].error() != simdjson::SUCCESS) {
                    break;
                }
                std::string_view code_str = doc["code"];
                if (code_str != SUCCESS_CODE) {
                    break;
                }
                Currency currency("USD");
                parse_pairs_info(doc, currency);
                INFRA_LOG_INFO("[edgex] [fetch_pairs_info] [success], pairs_info size: {}, stark_info size: {}",
                               g_pairs_info_cache.size(), g_stark_info_cache.size());
                cb(Errno::Ok, g_pairs_info_cache);
                return;
            } catch (const std::exception& ex) {
                INFRA_LOG_WARN("[edgex] [fetch_pairs_info] [exception], msg: {}", ex.what());
            }
        } while (0);
        INFRA_LOG_WARN("[edgex] [fetch_pairs_info] [fail], recv: {}", response);
        cb(extract_error_code(response), {});
    });
}

void EdgexMarketData::fetch_pairs_info_sync() {
    auto req = get_request_body(rest_host_, pairs_info_path_);
    boost::beast::error_code ec;
    std::string msg = rest_.sync_send(req, ec);
    do {
        if (ec) {
            break;
        }
        try {
            PARSE_JSON(msg, doc);
            if (doc["code"].error() != simdjson::SUCCESS) {
                break;
            }
            std::string_view code_str = doc["code"];
            if (code_str != SUCCESS_CODE) {
                break;
            }
            Currency currency("USD");
            parse_pairs_info(doc, currency);
            INFRA_LOG_INFO("[edgex] [fetch_pairs_info] [success], pairs_info size: {}, stark_info size: {}",
                           g_pairs_info_cache.size(), g_stark_info_cache.size());
            return;
        } catch (const std::exception& ex) {
            INFRA_LOG_WARN("[edgex] [fetch_pairs_info] [exception], msg: {}", ex.what());
        }
    } while (0);
    INFRA_LOG_WARN("[edgex] [fetch_pairs_info] [fail], recv: {}", msg);
}

void EdgexMarketData::fetch_funding_fee(const Symbol& symbol, FundingFeeCallback cb) {
    if (funding_fee_path_.empty()) {
        cb(Errno::NotImplemented, nullptr);
        return;
    }

    if (symbol.empty()) {
        cb(Errno::InvalidParams, nullptr);
        return;
    }

    std::string contractId;
    if (!get_contract_id(symbol, contractId)) {
        INFRA_LOG_WARN("[edgex] [fetch_funding_fee] [fail], msg: get contract id failed for pair {}", symbol);
        cb(Errno::InvalidParams, nullptr);
        return;
    }

    std::string query = "contractId=" + contractId;
    auto req = get_request_body(rest_host_, funding_fee_path_, query);
    rest_.send(req, [cb](HttpResponseBody& res) {
        std::string response = boost::beast::buffers_to_string(res.body().data());
        do {
            if (res.result() != HTTP_STATUS_OK) {
                break;
            }
            try {
                PARSE_JSON(response, doc);
                if (doc["code"].error() != simdjson::SUCCESS) {
                    break;
                }
                std::string_view code_str = doc["code"];
                if (code_str != SUCCESS_CODE) {
                    break;
                }
                INFRA_LOG_INFO("[edgex] [fetch_funding_fee] [success], recv: {}", response);
                auto fee = parse_funding_fee(doc);
                cb(Errno::Ok, fee);
                return;
            } catch (const std::exception& ex) {
                INFRA_LOG_WARN("[edgex] [fetch_funding_fee] [exception], msg: {}", ex.what());
            }
        } while (0);
        INFRA_LOG_WARN("[edgex] [fetch_funding_fee] [fail], recv: {}", response);
        cb(extract_error_code(response), nullptr);
    });
}

Action EdgexMarketData::on_connect(Wss* ws) {
    size_t index = ws->get_index();
    INFRA_LOG_INFO("[edgex] [on_connect] [MarketData], msg: WebSocket connection established, index: {}", index);
    if (index < wss_connections_.size()) {
        auto client = wss_connections_[index];
        subscribe(index);
    } else {
        INFRA_LOG_WARN("[edgex] [on_connect] [fail], msg: invalid connection ID {}", index);
    }
    return Action::NONE;
}

Action EdgexMarketData::on_ping(Wss* ws, std::string_view payload) {
    // INFRA_LOG_DEBUG("[edgex] [on_ping] [MarketData], payload: {}", payload);
    ws->pong(std::string(payload));
    return Action::NONE;
}

Action EdgexMarketData::on_pong(Wss* ws, std::string_view payload) {
    // INFRA_LOG_DEBUG("[edgex] [on_pong] [MarketData], payload: {}", payload);
    return Action::NONE;
}

void EdgexMarketData::on_close(Wss* ws) {
    size_t index = ws->get_index();
    INFRA_LOG_WARN("[edgex] [on_close] [MarketData], msg: WebSocket connection has been closed, index: {}", index);
}

void EdgexMarketData::on_error(Wss* ws, std::string_view err) {
    size_t index = ws->get_index();
    INFRA_LOG_WARN("[edgex] [on_error] [MarketData], msg: WebSocket error occurred: {}, index: {}", err, index);
}

Action EdgexMarketData::on_message(Wss* ws, std::string_view msg) {
    // INFRA_LOG_INFO("[edgex] [on_message] [MarketData], msg: {}", msg);
    uint64_t recv_tsc = rdtsc();
    uint64_t recv_milli = time_get_now_milli();
    try {
        PARSE_JSON(msg, doc);
        if (doc["content"].error() == simdjson::SUCCESS) {
            simdjson::dom::object content = doc["content"].get_object();
            std::string_view channel = content["channel"];
            if (content["data"].error() == simdjson::SUCCESS) {
                simdjson::dom::array data_array = content["data"].get_array();
                for (auto data : data_array) {
                    if (channel.find("depth") == 0) {
                        on_message_bookticker(data, recv_tsc, recv_milli);
                    } else {
                        INFRA_LOG_WARN("[edgex] [on_message] [warn], msg: unknown channel {}", channel);
                    }
                }
            } else {
                INFRA_LOG_WARN("[edgex] [on_message] [MarketData] unexpected msg: {}", msg);
            }
        } else if (doc["type"].error() == simdjson::SUCCESS) {
            std::string_view type = doc["type"];
            if (type == "connected" || type == "subscribed") {
                INFRA_LOG_INFO("[edgex] [on_message] [MarketData] msg: {}", msg);
            } else if (type == "ping") {
                // ignore
            } else {
                INFRA_LOG_WARN("[edgex] [on_message] [MarketData] unexpected msg: {}", msg);
            }
        } else {
            INFRA_LOG_WARN("[edgex] [on_message] [MarketData] unexpected msg: {}", msg);
        }
    } catch (const std::exception& ex) {
        INFRA_LOG_WARN("[edgex] [on_message] [exception], error: {}, msg: {}", ex.what(), msg);
    }
    return Action::RECEIVE;
}

void EdgexMarketData::subscribe(size_t index) {
    size_t delay_s = index + 1; // NOTE：延迟订阅，提高成功率
    auto timer = std::make_shared<boost::asio::steady_timer>(ioc_, std::chrono::seconds(delay_s));
    timer->async_wait([this, index, timer](const boost::system::error_code& ec) {
        for (std::string payload : stream_params_[index]) {
            INFRA_LOG_INFO("[edgex] [subscribe_orderbook], connection {}, send: {}", index, payload);
            wss_connections_[index]->send(std::move(payload));
        }
    });
}

void EdgexMarketData::on_message_bookticker(const simdjson::dom::object& data, uint64_t recv_tsc, uint64_t recv_milli) {
    std::string_view contractId = data["contractId"];
    std::string symbol;
    if (!get_symbol_by_contract_id(std::string(contractId), symbol)) {
        INFRA_LOG_WARN("[edgex] [on_message_bookticker] [fail], msg: get symbol failed for contractId {}", contractId);
        return;
    }

    double best_ask_price = 0.0;
    double best_ask_size = 0.0;
    double best_bid_price = 0.0;
    double best_bid_size = 0.0;
    simdjson::dom::array bids_array = data["bids"].get_array();
    for (auto&& items : bids_array) {
        std::string_view price_text = items["price"];
        std::string_view amount_text = items["size"];
        double price = str_to_float(price_text);
        double amount = str_to_float(amount_text);
        best_bid_price = price;
        best_bid_size = amount;
        break;
    }
    simdjson::dom::array asks_array = data["asks"].get_array();
    for (auto&& items : asks_array) {
        std::string_view price_text = items["price"];
        std::string_view amount_text = items["size"];
        double price = str_to_float(price_text);
        double amount = str_to_float(amount_text);
        best_ask_price = price;
        best_ask_size = amount;
        break;
    }
    SpOrderBook orderbook = this->apply_orderbook_delta(symbol, time_get_now_milli(), best_ask_price, best_ask_size,
                                                        best_bid_price, best_bid_size);
    orderbook->recv_tsc = recv_tsc;
    orderbook->recv_milli = recv_milli;
    orderbook->parsed_tsc = rdtsc();
    this->dispatch_orderbook(std::move(orderbook));
}
} // namespace infra