#include "gate_market_data.h"
#include "gate_sbe/messages/bbo.hpp"
using namespace infra::gate;

namespace infra {
bool GateMarketData::initialize() {
    auto& info = g_config_map[base_config_.to_str()];
    if (info.empty()) {
        INFRA_LOG_WARN("[gate] [initialize] [fail], msg: {} {} {} not implemented",
                       to_string(base_config_.account_type), to_string(base_config_.address_type),
                       to_string(base_config_.settle_unit));
        return false;
    }

    rest_host_ = info[REST_HOST];
    pairs_info_path_ = info[PAIRS_INFO_PATH];
    funding_fee_path_ = info[FUNDING_FEE_PATH];

    wss_infos_ = {info[WSS_PUBLIC_HOST], info[WSS_PORT], info[WSS_PUBLIC_PATH]};
    INFRA_LOG_INFO("[gate] [initialize] [MarketData], websocket endpoint: {} {} {}", wss_infos_.host, wss_infos_.path,
                   wss_infos_.port);
    return true;
}

void GateMarketData::shutdown() { unsubscribe_orderbook(); }

bool GateMarketData::subscribe_orderbook(const Symbols& symbols, unsigned int depth, OrderbookCallback cb) {
    constexpr size_t MAX_STREAMS_PER_WSS_CONNECTION = 80;

    this->orderbook_handler_ = std::move(cb);
    const Symbols& sub_symbols = (!symbols.empty()) ? symbols : g_all_symbols;
    stream_params_.clear();
    std::vector<std::string> current_batch;
    size_t total = sub_symbols.size();

    for (size_t i = 0; i < total; ++i) {
        std::string symbol = transfer_from_infra_pair(sub_symbols[i]);
        current_batch.push_back(symbol);
        if (current_batch.size() >= MAX_STREAMS_PER_WSS_CONNECTION || i == total - 1) {
            std::string payload_str = "[";
            for (size_t j = 0; j < current_batch.size(); ++j) {
                if (j > 0)
                    payload_str += ",";
                payload_str += fmt::format("\"{}\"", current_batch[j]);
            }
            payload_str += "]";
            std::string subscribe_msg =
                fmt::format(R"({{"time":{},"channel":"futures.book_ticker","event":"subscribe","payload":{} }})",
                            time_get_now_sec(), payload_str);

            stream_params_.push_back({std::move(subscribe_msg)});
            current_batch.clear();
        }
    }

    INFRA_LOG_INFO("[gate] [subscribe_bookticker], establishing {} connections", stream_params_.size());
    for (size_t i = 0; i < stream_params_.size(); i++) {
        auto tmp = std::make_shared<WebSocketClient>(ioc_, ssl_ctx_, *this);
        wss_connections_.push_back(tmp);
        auto& conn = wss_connections_.back();
        conn->set_user_data(i); // index of wss_connections_
        conn->set_ws_header_field(std::bind(&GateMarketData::add_header, this, std::placeholders::_1));
        if (g_use_sbe) {
            conn->resolve_connect(wss_infos_.host, wss_infos_.port, "/v4/ws/usdt/sbe?sbe_schema_id=1");
        } else {
            conn->resolve_connect(wss_infos_.host, wss_infos_.port, wss_infos_.path);
        }
    }
    return true;
}

void GateMarketData::add_header(websocket::request_type& req) { req.set("X-Gate-Size-Decimal", "1"); }

void GateMarketData::unsubscribe_orderbook() {
    this->orderbook_handler_ = nullptr;
    for (auto& conn : wss_connections_) {
        conn->close();
    }
    stream_params_.clear();
    wss_connections_.clear();
    INFRA_LOG_INFO("[gate] [unsubscribe_orderbook] [success]");
}

void GateMarketData::fetch_pairs_info(ExPairInfoCallback cb) {
    auto req = get_request_body(rest_host_, pairs_info_path_);
    req.set("X-Gate-Size-Decimal", "1");
    client_.send(req, [this, cb](HttpResponseBody& res) {
        std::string msg = boost::beast::buffers_to_string(res.body().data());
        handle_rest_response(
            res, msg, "fetch_pairs_info",
            [&](auto& doc) {
                if (!doc.is_array())
                    return false;
                Currency currency = to_string(base_config_.settle_unit);
                parse_pairs_info(doc, currency);
                INFRA_LOG_INFO("[gate] [fetch_pairs_info] [success], size: {}", g_pairs_info_cache.size());
                cb(Errno::Ok, g_pairs_info_cache);
                return true;
            },
            [&]() { cb(extract_error_code(msg), {}); });
    });
}

void GateMarketData::fetch_funding_fee(const Symbol& symbol, FundingFeeCallback cb) {
    if (symbol.empty()) {
        cb(Errno::InvalidParams, nullptr);
        return;
    }

    std::string query = fmt::format("contract={}&limit=1", transfer_from_infra_pair(symbol));
    auto req = get_request_body(rest_host_, funding_fee_path_, query);
    client_.send(req, [this, symbol, cb](HttpResponseBody& res) {
        std::string msg = boost::beast::buffers_to_string(res.body().data());
        handle_rest_response(
            res, msg, "fetch_funding_fee",
            [&](auto& doc) {
                if (!doc.is_array())
                    return false;
                INFRA_LOG_INFO("[gate] [fetch_funding_fee] [success], recv: {}", msg);
                auto funding_fee = parse_funding_fee(doc, symbol);
                cb(Errno::Ok, funding_fee);
                return true;
            },
            [&]() { cb(extract_error_code(msg), nullptr); });
    });
}

Action GateMarketData::on_connect(Wss* ws) {
    size_t index = ws->get_index();
    INFRA_LOG_INFO("[gate] [on_connect] [MarketData], connection id: {}", index);
    subscribe(index);
    return Action::NONE;
}

Action GateMarketData::on_ping(Wss* ws, std::string_view payload) {
    ws->pong(std::string(payload));
    return Action::NONE;
}

Action GateMarketData::on_pong(Wss* ws, std::string_view payload) { return Action::NONE; }

void GateMarketData::on_close(Wss* ws) {
    size_t index = ws->get_index();
    INFRA_LOG_WARN("[gate] [on_close] [MarketData], connection id: {}", index);
}

void GateMarketData::on_error(Wss* ws, std::string_view err) {
    size_t index = ws->get_index();
    INFRA_LOG_WARN("[gate] [on_error] [MarketData], connection id: {}, err: {}", index, err);
}

Action GateMarketData::on_message(Wss* ws, std::string_view msg) {
    uint64_t recv_tsc = rdtsc();
    uint64_t recv_milli = time_get_now_milli();
    char first = msg[0];
    if (first != '{') {
        on_message_bookticker_sbe(msg, recv_tsc, recv_milli);
        return Action::RECEIVE;
    }

    try {
        PARSE_JSON(msg, doc);
        std::string_view channel = doc["channel"];
        std::string_view event = doc["event"];
        if (channel == "futures.book_ticker" && event == "update") {
            on_message_bookticker(doc["result"], recv_tsc, recv_milli);
        } else if (event == "subscribe") {
            INFRA_LOG_INFO("[gate] [subscribe_orderbook] [success], recv: {}", msg);
        } else {
            INFRA_LOG_WARN("[gate] [on_message] [MarketData], unexpected msg: {}", msg);
        }
    } catch (const std::exception& ex) {
        INFRA_LOG_WARN("[gate] [on_message] [MarketData], msg: {}, ex: {}", ex.what(), msg);
    }
    return Action::RECEIVE;
}

void GateMarketData::subscribe(size_t index) {
    for (const std::string& payload : stream_params_[index]) {
        INFRA_LOG_INFO("[gate] [subscribe_orderbook], connection {}, send: {}", index, payload);
        wss_connections_[index]->send(payload);
    }
}

void GateMarketData::on_message_bookticker(const simdjson::dom::object& data, uint64_t recv_tsc, uint64_t recv_milli) {
    std::string_view symbol = data["s"];
    Symbol pair = transfer_to_infra_pair(symbol);
    Timestamp milli = data["t"];

    std::string_view ask_price_sv = data["a"];
    std::string_view bid_price_sv = data["b"];
    std::string_view ask_size_sv = data["A"];
    std::string_view bid_size_sv = data["B"];

    double best_ask_price = str_to_float(ask_price_sv);
    double best_bid_price = str_to_float(bid_price_sv);
    double best_ask_size = str_to_float(ask_size_sv) * g_pairs_info_cache[pair]->denomination_value;
    double best_bid_size = str_to_float(bid_size_sv) * g_pairs_info_cache[pair]->denomination_value;

    SpOrderBook orderbook =
        this->apply_orderbook_delta(pair, milli, best_ask_price, best_ask_size, best_bid_price, best_bid_size);
    orderbook->recv_tsc = recv_tsc;
    orderbook->recv_milli = recv_milli;
    orderbook->parsed_tsc = rdtsc();
    this->dispatch_orderbook(std::move(orderbook));
}

void GateMarketData::on_message_bookticker_sbe(std::string_view msg, uint64_t recv_tsc, uint64_t recv_milli) {
    auto bbo = sbepp::make_view<gate_sbe::messages::bbo>(msg.data(), msg.size());
    int64_t ts = bbo.t().value();
    int8_t price_exponent = bbo.pxExponent().value();
    int8_t size_exponent = bbo.szExponent().value();
    int64_t ask_mant_price = bbo.askMantissaPrice().value();
    int64_t ask_mant_size = bbo.askMantissaSize().value();
    int64_t bid_mant_price = bbo.bidMantissaPrice().value();
    int64_t bid_mant_size = bbo.bidMantissaSize().value();
    std::string symbol(reinterpret_cast<const char*>(bbo.s().data()), bbo.s().size());

    Timestamp milli = ts / 1000;
    Symbol pair = transfer_to_infra_pair(symbol);
    double denomination = g_pairs_info_cache[pair]->denomination_value; // 合约张数转币数

    double best_ask_price = calc_decimal_sbe(ask_mant_price, price_exponent);
    double best_ask_size = calc_decimal_sbe(ask_mant_size, size_exponent) * denomination;
    double best_bid_price = calc_decimal_sbe(bid_mant_price, price_exponent);
    double best_bid_size = calc_decimal_sbe(bid_mant_size, size_exponent) * denomination;

    SpOrderBook orderbook =
        this->apply_orderbook_delta(pair, milli, best_ask_price, best_ask_size, best_bid_price, best_bid_size);
    orderbook->recv_tsc = recv_tsc;
    orderbook->recv_milli = recv_milli;
    orderbook->parsed_tsc = rdtsc();
    this->dispatch_orderbook(std::move(orderbook));
}
} // namespace infra