#pragma once
#include "kucoin_utils.h"
#include "network/websocket.h"
namespace infra {
class KucoinMarketData : public IExchangeMarketData, public WssHandler {
public:
    KucoinMarketData(net::io_context& ioc, ssl::context& ssl_ctx, const AccountSecret& sec, APIConfig config)
        : IExchangeMarketData(ioc, ssl_ctx, sec, config), rest_(ioc_, ssl_ctx_) {}
    ~KucoinMarketData() override = default;

    bool initialize() override;
    void shutdown() override;

    bool subscribe_orderbook(const Symbols& symbols, unsigned int depth, OrderbookCallback cb) override;
    void unsubscribe_orderbook() override;

    void fetch_pairs_info(ExPairInfoCallback cb) override;
    void fetch_funding_fee(const Symbol& symbol, FundingFeeCallback cb) override;

public:
    Action on_connect(Wss* ws) override;
    Action on_ping(Wss* ws, std::string_view payload) override;
    Action on_pong(Wss* ws, std::string_view payload) override;
    void on_close(Wss* ws) override;
    void on_error(Wss* ws, std::string_view err) override;
    Action on_message(Wss* ws, std::string_view msg) override;

private:
    /* unified */
    void subscribe_unified(size_t index);
    void on_message_unified_orderbook(const simdjson::dom::object& data, uint64_t recv_tsc, uint64_t recv_milli);
    bool subscribe_unified_orderbook(const Symbols& symbols, unsigned int depth, OrderbookCallback cb);
    Action on_unified_connect(Wss* ws);
    Action on_unified_message(Wss* ws, std::string_view msg);
    void fetch_unified_pairs_info(ExPairInfoCallback cb);
    void fetch_unified_funding_fee(const Symbol& symbol, FundingFeeCallback cb);

    using WebSocketClient = WssClient<KucoinMarketData>;
    inline void keep_ws_connection_alive(WebSocketClient& client) {
        std::string msg = fmt::format(R"({{"id":"{}","type":"ping"}})", time_get_now_micro());
        client.start_ping_pong(msg, 15); // 心跳检测时间为20秒
    }

private:
    HttpClient rest_;
    std::string rest_host_{};
    std::string pairs_info_path_{};
    std::string funding_fee_path_{};

    ConnectData wss_infos_;

    std::vector<std::shared_ptr<WebSocketClient>> wss_connections_;
    std::vector<std::string> stream_params_;
};
} // namespace infra
