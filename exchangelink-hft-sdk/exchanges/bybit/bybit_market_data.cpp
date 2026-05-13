#include "bybit_market_data.h"
using namespace infra::bybit;

namespace infra {
bool BybitMarketData::initialize() {
    auto& info = g_config_map[base_config_.to_str()];
    if (info.empty()) {
        INFRA_LOG_WARN("[bybit] [initialize] [fail], msg: {} {} {} not implemented",
                       to_string(base_config_.account_type), to_string(base_config_.address_type),
                       to_string(base_config_.settle_unit));
        return false;
    }

    rest_host_ = info[REST_HOST];
    pairs_info_path_ = info[PAIRS_INFO_PATH];
    funding_fee_path_ = info[FUNDING_FEE_PATH];
    category_ = "linear";
    wss_infos_ = {info[WSS_PUBLIC_HOST], info[WSS_PORT], info[WSS_PUBLIC_PATH]};
    INFRA_LOG_INFO("[bybit] [initialize] [MarketData], websocket endpoint: {} {} {}", wss_infos_.host, wss_infos_.path,
                   wss_infos_.port);
    return true;
}

void BybitMarketData::shutdown() { unsubscribe_orderbook(); }

bool BybitMarketData::subscribe_orderbook(const Symbols& symbols, unsigned int depth, OrderbookCallback cb) {
    if (depth != 1) {
        INFRA_LOG_WARN("[bybit] [subscribe_orderbook] [fail], msg: unsupported depth level {}", depth);
        return false;
    }

    this->orderbook_handler_ = std::move(cb);
    const Symbols& sub_symbols = (!symbols.empty()) ? symbols : g_all_symbols;
    stream_params_.clear();
    std::vector<std::string> current_batch_args;
    size_t total = sub_symbols.size();
    for (size_t i = 0; i < total; ++i) {
        std::string symbol_upper = transfer_from_infra_pair(sub_symbols[i]);
        std::string topic = "orderbook." + std::to_string(depth) + "." + symbol_upper;
        current_batch_args.push_back("\"" + topic + "\"");
        if (current_batch_args.size() >= MAX_PAIRS_PER_WS_CONNECTION || i == total - 1) {
            std::ostringstream oss;
            oss << R"({"op":"subscribe","args":[)";
            for (size_t k = 0; k < current_batch_args.size(); ++k) {
                oss << current_batch_args[k];
                if (k < current_batch_args.size() - 1) {
                    oss << ",";
                }
            }
            oss << "]}";

            stream_params_.push_back(oss.str());
            current_batch_args.clear();
        }
    }

    INFRA_LOG_INFO("[bybit] [subscribe_orderbook], establishing {} WebSocket connections", stream_params_.size());
    for (size_t i = 0; i < stream_params_.size(); i++) {
        auto client = std::make_shared<WebSocketClient>(ioc_, ssl_ctx_, *this);
        wss_connections_.push_back(client);
        auto& conn = wss_connections_.back();
        conn->set_user_data(i); // index of wss_connections_
        conn->resolve_connect(wss_infos_.host, wss_infos_.port, wss_infos_.path);
    }
    return true;
}

void BybitMarketData::unsubscribe_orderbook() {
    this->orderbook_handler_ = nullptr;
    for (auto& conn : wss_connections_) {
        if (conn->is_socket_open()) {
            conn->close();
        }
    }
    stream_params_.clear();
    wss_connections_.clear();
    INFRA_LOG_INFO("[bybit] [unsubscribe_orderbook] [success]");
}

void BybitMarketData::fetch_pairs_info(ExPairInfoCallback cb) {
    if (pairs_info_path_.empty()) {
        cb(Errno::NotImplemented, {});
        return;
    }

    std::string query = "category=" + category_ + "&status=Trading&limit=1000";
    auto req = get_request_body(rest_host_, pairs_info_path_, query);
    rest_.send(req, [this, cb](HttpResponseBody& res) {
        std::string response = boost::beast::buffers_to_string(res.body().data());
        do {
            if (res.result() != HTTP_STATUS_OK) {
                break;
            }
            try {
                PARSE_JSON(response, doc);
                if (doc["retCode"].get_int64() != BYBIT_SUCCESS_CODE) {
                    break;
                }
                Currency currency = to_string(base_config_.settle_unit);
                parse_pairs_info(doc, currency);
                INFRA_LOG_INFO("[bybit] [fetch_pairs_info] [success], size: {}", g_pairs_info_cache.size());
                cb(Errno::Ok, g_pairs_info_cache);
                return;
            } catch (const std::exception& ex) {
                INFRA_LOG_WARN("[bybit] [fetch_pairs_info] [exception], msg: {}", ex.what());
            }
        } while (0);
        INFRA_LOG_WARN("[bybit] [fetch_pairs_info] [fail], recv: {}", response);
        cb(extract_error_msg(response), {});
    });
}

void BybitMarketData::fetch_funding_fee(const Symbol& symbol, FundingFeeCallback cb) {
    if (funding_fee_path_.empty()) {
        cb(Errno::NotImplemented, nullptr);
        return;
    }

    if (symbol.empty()) {
        cb(Errno::InvalidParams, nullptr);
        return;
    }

    Symbol pair = to_exchange_pair(Exchange::BYBIT, symbol);
    std::string query = "category=" + category_ + "&symbol=" + pair + "&limit=1";
    auto req = get_request_body(rest_host_, funding_fee_path_, query);
    rest_.send(req, [cb](HttpResponseBody& res) {
        std::string response = boost::beast::buffers_to_string(res.body().data());
        do {
            if (res.result() != HTTP_STATUS_OK) {
                break;
            }
            try {
                PARSE_JSON(response, doc);
                if (doc["retCode"].get_int64() != BYBIT_SUCCESS_CODE) {
                    break;
                }
                INFRA_LOG_INFO("[bybit] [fetch_funding_fee] [success], recv: {}", response);
                auto fee = parse_funding_fee(doc);
                cb(Errno::Ok, fee);
                return;
            } catch (const std::exception& ex) {
                INFRA_LOG_WARN("[bybit] [fetch_funding_fee] [exception], msg: {}", ex.what());
            }
        } while (0);
        INFRA_LOG_WARN("[bybit] [fetch_funding_fee] [fail], recv: {}", response);
        cb(extract_error_msg(response), nullptr);
    });
}

Action BybitMarketData::on_connect(Wss* ws) {
    size_t index = ws->get_index();
    INFRA_LOG_INFO("[bybit] [on_connect] [MarketData], msg: WebSocket connection established, index: {}", index);
    keep_ws_connection_alive(index);
    subscribe(index);
    return Action::NONE;
}

Action BybitMarketData::on_ping(Wss* ws, std::string_view payload) {
    // INFRA_LOG_DEBUG("[bybit] [on_ping] [MarketData], payload: {}", payload);
    ws->pong(std::string(payload));
    return Action::NONE;
}

Action BybitMarketData::on_pong(Wss* ws, std::string_view payload) {
    // INFRA_LOG_DEBUG("[bybit] [on_pong] [MarketData], payload: {}", payload);
    return Action::NONE;
}

void BybitMarketData::on_close(Wss* ws) {
    INFRA_LOG_WARN("[bybit] [on_close] [MarketData], msg: WebSocket connection has been closed, index: {}",
                   ws->get_index());
}

void BybitMarketData::on_error(Wss* ws, std::string_view err) {
    INFRA_LOG_WARN("[bybit] [on_error] [MarketData], msg: WebSocket error occurred: {}, index: {}", err,
                   ws->get_index());
}

Action BybitMarketData::on_message(Wss* ws, std::string_view msg) {
    INFRA_LOG_INFO("[bybit] [on_message] [MarketData], msg: {}", msg);
    try {
        PARSE_JSON(msg, doc);
        if (doc["topic"].error() == simdjson::SUCCESS) {
            std::string_view topic = doc["topic"];
            if (topic.find("orderbook") != std::string_view::npos) {
                int64_t ts = doc["ts"];
                std::string_view type = doc["type"];
                simdjson::dom::object data = doc["data"];
                if (type == "snapshot") {
                    on_message_orderbook(data, ts, true);
                } else if (type == "delta") {
                    on_message_orderbook(data, ts, false);
                }
            } else {
                INFRA_LOG_WARN("[bybit] [on_message] unexpected msg: {}", msg);
            }
        } else if (doc["op"].error() == simdjson::SUCCESS) {
            std::string_view op = doc["op"];
            if (op == "subscribe") {
                bool success = doc["success"];
                if (success) {
                    INFRA_LOG_INFO("[bybit] [subscribe_orderbook] [success], msg: {}", msg);
                } else {
                    INFRA_LOG_WARN("[bybit] [subscribe_orderbook] [fail], msg: {}", msg);
                }
            } else if (op == "ping") {
                // ignore
            } else {
                INFRA_LOG_WARN("[bybit] [on_message] unexpected msg: {}", msg);
            }
        } else {
            INFRA_LOG_WARN("[bybit] [on_message] unexpected msg: {}", msg);
        }
    } catch (const std::exception& ex) {
        INFRA_LOG_WARN("[bybit] [on_message] [exception], error: {}, msg: {}", ex.what(), msg);
    }
    return Action::RECEIVE;
}

void BybitMarketData::keep_ws_connection_alive(size_t index) {
    wss_connections_[index]->start_ping_pong(R"({"op":"ping"})", 25); // 心跳检测时间为30秒
}

void BybitMarketData::subscribe(size_t index) {
    std::string payload = stream_params_[index];
    INFRA_LOG_INFO("[bybit] [subscribe_orderbook], connection index: {}, send: {}", index, payload);
    wss_connections_[index]->send(std::move(payload));
}

void BybitMarketData::on_message_orderbook(const simdjson::dom::object& data, int64_t ts, bool is_snapshot) {
    std::string_view symbol = data["s"];
    Symbol pair = transfer_to_infra_pair(symbol);
    int64_t update_id = data["u"];
    Timestamp milli = ts;

    // auto asks = data["a"].get_array();
    // for (auto&& items : asks) {
    //     auto it = items.begin();
    //     bfloat price = str_to_float(std::string_view(*it));
    //     ++it;
    //     bfloat amount = str_to_float(std::string_view(*it)) * denomination;
    //     levels.emplace_back(price, amount);
    //     break;
    // }

    // auto bids = data["b"].get_array();
    // for (auto&& items : bids) {
    //     auto it = items.begin();
    //     bfloat price = str_to_float(std::string_view(*it));
    //     ++it;
    //     bfloat amount = str_to_float(std::string_view(*it)) * denomination;
    //     levels.emplace_back(price, amount);
    //     break;
    // }

    // SpOrderBook orderbook = this->apply_orderbook_delta(is_snapshot, pair, milli, asks, bids, update_id);
    // this->dispatch_orderbook(std::move(orderbook));
}
} // namespace infra