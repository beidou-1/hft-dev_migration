#include "bitget_market_data.h"
#include "bitget_sbe/messages/BestBidAsk.hpp"
using namespace infra::bitget;

namespace infra {
bool BitgetMarketData::initialize() {
    auto& info = g_config_map[base_config_.to_str()];
    if (info.empty()) {
        INFRA_LOG_WARN("[bitget] [initialize] [fail], msg: {} {} {} not implemented",
                       to_string(base_config_.account_type), to_string(base_config_.address_type),
                       to_string(base_config_.settle_unit));
        return false;
    }

    rest_host_ = info[REST_HOST];
    pairs_info_path_ = info[PAIRS_INFO_PATH];
    funding_fee_path_ = info[FUNDING_FEE_PATH];

    wss_infos_ = {info[WSS_PUBLIC_HOST], info[WSS_PORT], info[WSS_PUBLIC_PATH]};
    INFRA_LOG_INFO("[bitget] [initialize] [MarketData], websocket endpoint: {} {} {}", wss_infos_.host, wss_infos_.path,
                   wss_infos_.port);
    return true;
}

void BitgetMarketData::shutdown() { unsubscribe_orderbook(); }

bool BitgetMarketData::subscribe_orderbook(const Symbols& symbols, unsigned int depth, OrderbookCallback cb) {
    this->orderbook_handler_ = std::move(cb);
    size_t MAX_PAIRS_PER_WS_CONNECTION = 60; // 单个连接订阅个数
    const Symbols& sub_symbols = (!symbols.empty()) ? symbols : g_all_symbols;
    size_t total = sub_symbols.size();

    stream_params_.clear();
    std::ostringstream oss{};
    for (size_t i = 1; i <= total; i++) {
        std::string pair = transfer_from_infra_pair(sub_symbols[i - 1]);
        oss << "{\"instType\":\"usdt-futures\",\"topic\":\"books1\",\"symbol\":\"" << pair << "\"},";
        if (i % MAX_PAIRS_PER_WS_CONNECTION == 0 || i == total) {
            std::string params = oss.str();
            params.pop_back(); // remove last ','
            stream_params_.push_back(std::move(params));
            oss.str("");
        }
    }

    wss_connections_.clear();
    parser_.resize(stream_params_.size());
    for (size_t i = 0; i < stream_params_.size(); i++) {
        auto tmp = std::make_shared<WebSocketClient>(ioc_, ssl_ctx_, *this);
        wss_connections_.push_back(tmp);
        auto& conn = wss_connections_.back();
        conn->set_user_data(i); // index of wss_connections_
        conn->resolve_connect(wss_infos_.host, wss_infos_.port, wss_infos_.path);
    }
    INFRA_LOG_INFO("[bitget] [subscribe_orderbook], establishing {} connections", stream_params_.size());
    return true;
}

void BitgetMarketData::unsubscribe_orderbook() {
    this->orderbook_handler_ = nullptr;
    for (auto& conn : wss_connections_) {
        conn->close();
    }
    INFRA_LOG_INFO("[bitget] [unsubscribe_orderbook] [success]");
}

void BitgetMarketData::fetch_pairs_info(ExPairInfoCallback cb) {
    std::string query = "category=USDT-FUTURES";
    auto req = get_request_body(rest_host_, pairs_info_path_, query);
    client_.send(req, [this, cb](HttpResponseBody& res) {
        std::string msg = boost::beast::buffers_to_string(res.body().data());
        handle_rest_response(
            res, msg, "fetch_pairs_info",
            [&](auto& doc) {
                Currency currency = to_string(base_config_.settle_unit);
                parse_pairs_info(doc, currency);
                INFRA_LOG_INFO("[bitget] [fetch_pairs_info] [success], size: {}", g_pairs_info_cache.size());
                cb(Errno::Ok, g_pairs_info_cache);
                return true;
            },
            [&]() { cb(extract_error_code(msg), {}); });
    });
}

void BitgetMarketData::fetch_funding_fee(const Symbol& symbol, FundingFeeCallback cb) {
    if (symbol.empty()) {
        cb(Errno::InvalidParams, nullptr);
        return;
    }

    std::string query = "symbol=" + transfer_from_infra_pair(symbol);
    auto req = get_request_body(rest_host_, funding_fee_path_, query);
    client_.send(req, [this, symbol, cb](HttpResponseBody& res) {
        std::string msg = boost::beast::buffers_to_string(res.body().data());
        handle_rest_response(
            res, msg, "fetch_funding_fee",
            [&](auto& doc) {
                INFRA_LOG_INFO("[bitget] [fetch_funding_fee] [success], recv: {}", msg);
                SpFundingFee funding_fee = std::make_shared<FundingFee>(symbol);
                parse_funding_fee(doc, funding_fee);
                cb(Errno::Ok, funding_fee);
                return true;
            },
            [&]() { cb(extract_error_code(msg), nullptr); });
    });
}

Action BitgetMarketData::on_connect(Wss* ws) {
    size_t index = ws->get_index();
    INFRA_LOG_INFO("[bitget] [on_connect] [MarketData], connection id: {}", index);
    subscribe(index);
    keep_ws_connection_alive(index);
    return Action::NONE;
}

Action BitgetMarketData::on_ping(Wss* ws, std::string_view payload) {
    ws->pong(std::string(payload));
    return Action::NONE;
}

Action BitgetMarketData::on_pong(Wss* ws, std::string_view payload) { return Action::NONE; }

void BitgetMarketData::on_close(Wss* ws) {
    size_t index = ws->get_index();
    INFRA_LOG_WARN("[bitget] [on_close] [MarketData], connection id: {}", index);
}

void BitgetMarketData::on_error(Wss* ws, std::string_view err) {
    size_t index = ws->get_index();
    INFRA_LOG_WARN("[bitget] [on_error] [MarketData], connection id: {}, err: {}", index, err);
}

Action BitgetMarketData::on_message(Wss* ws, std::string_view msg) {
    uint64_t recv_tsc = rdtsc();
    uint64_t recv_milli = time_get_now_milli();
    char first = msg[0];
    if (first != '{') { // 过滤非json响应，例如"pong"
        if (msg != "pong") [[likely]] {
            on_message_bookticker_sbe(msg, recv_tsc, recv_milli);
        }
        return Action::RECEIVE;
    }
    try {
        size_t index = ws->get_index();
        simdjson::padded_string simd_str(msg);
        simdjson::dom::element doc = parser_[index].parse(simd_str);
        if (doc["data"].error() == simdjson::SUCCESS) [[likely]] {
            std::string_view event = doc["action"];
            if (event == "snapshot") [[likely]] {
                on_message_bookticker(doc, recv_tsc, recv_milli);
            } else {
                INFRA_LOG_WARN("[bitget] [on_message] [MarketData], unexpected msg: {}", msg);
            }
        } else if (doc["event"].error() == simdjson::SUCCESS) {
            std::string_view event = doc["event"];
            if (event == "subscribe") {
                INFRA_LOG_INFO("[bitget] [subscribe_orderbook] [success], recv: {}", msg);
            } else {
                INFRA_LOG_WARN("[bitget] [on_message] [MarketData], unexpected msg: {}", msg);
            }
        } else {
            INFRA_LOG_WARN("[bitget] [on_message] [MarketData], unexpected msg: {}", msg);
        }
    } catch (const std::exception& ex) {
        INFRA_LOG_WARN("[bitget] [on_message] [MarketData], msg: {}, ex: {}", ex.what(), msg);
    }
    return Action::RECEIVE;
}

void BitgetMarketData::subscribe(size_t index) {
    std::string payload = R"({"op":"subscribe","args":[)" + stream_params_[index] + R"(]})";
    INFRA_LOG_INFO("[bitget] [subscribe_orderbook], connection {}, send: {}", index, payload);
    wss_connections_[index]->send(std::move(payload));
}

void BitgetMarketData::keep_ws_connection_alive(size_t index) { wss_connections_[index]->start_ping_pong("ping", 30); }

void BitgetMarketData::on_message_bookticker(const simdjson::dom::element& doc, uint64_t recv_tsc,
                                             uint64_t recv_milli) {
    std::string_view symbol = doc["arg"]["symbol"];
    Symbol pair = transfer_to_infra_pair(symbol);
    Timestamp milli = doc["ts"];

    simdjson::dom::array data = doc["data"];
    for (auto&& item : data) {
        auto it_a = item["a"].at(0).begin();
        std::string_view ask0_price_text = *it_a;
        ++it_a;
        std::string_view ask0_amount_text = *it_a;

        auto it_b = item["b"].at(0).begin();
        std::string_view bid0_price_text = *it_b;
        ++it_b;
        std::string_view bid0_amount_text = *it_b;

        double best_ask_price = str_to_float(ask0_price_text);
        double best_ask_vol = str_to_float(ask0_amount_text);
        double best_bid_price = str_to_float(bid0_price_text);
        double best_bid_vol = str_to_float(bid0_amount_text);

        SpOrderBook orderbook =
            this->apply_orderbook_delta(pair, milli, best_ask_price, best_ask_vol, best_bid_price, best_bid_vol);
        orderbook->recv_tsc = recv_tsc;
        orderbook->recv_milli = recv_milli;
        orderbook->parsed_tsc = rdtsc();
        this->dispatch_orderbook(std::move(orderbook));
    }
}

void BitgetMarketData::on_message_bookticker_sbe(std::string_view msg, uint64_t recv_tsc, uint64_t recv_milli) {
    auto bbo = sbepp::make_view<bitget_sbe::messages::BestBidAsk>(msg.data(), msg.size());
    int64_t ts = bbo.ts().value();
    int64_t bid_mant_price = bbo.bid1Price().value();
    int64_t bid_mant_size = bbo.bid1Size().value();
    int64_t ask_mant_price = bbo.ask1Price().value();
    int64_t ask_mant_size = bbo.ask1Size().value();
    int8_t price_exponent = bbo.priceExponent().value();
    int8_t size_exponent = bbo.sizeExponent().value();
    std::string symbol(reinterpret_cast<const char*>(bbo.symbol().data()), bbo.symbol().size());

    Timestamp milli = ts / 1000;
    Symbol pair = transfer_to_infra_pair(symbol);
    double best_ask_price = calc_decimal_sbe(ask_mant_price, price_exponent);
    double best_ask_size = calc_decimal_sbe(ask_mant_size, size_exponent);
    double best_bid_price = calc_decimal_sbe(bid_mant_price, price_exponent);
    double best_bid_size = calc_decimal_sbe(bid_mant_size, size_exponent);

    SpOrderBook orderbook =
        this->apply_orderbook_delta(pair, milli, best_ask_price, best_ask_size, best_bid_price, best_bid_size);
    orderbook->recv_tsc = recv_tsc;
    orderbook->recv_milli = recv_milli;
    orderbook->parsed_tsc = rdtsc();
    this->dispatch_orderbook(std::move(orderbook));
}
} // namespace infra