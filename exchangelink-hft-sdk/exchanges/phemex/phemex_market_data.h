#pragma once
#include "phemex_utils.h"
#include "network/websocket.h"
namespace infra {
class PhemexMarketData : public IExchangeMarketData, public WssHandler {
public:
    PhemexMarketData(net::io_context& ioc, ssl::context& ssl_ctx, const AccountSecret& sec, APIConfig config)
        : IExchangeMarketData(ioc, ssl_ctx, sec, config), rest_(ioc_, ssl_ctx_) {}
    ~PhemexMarketData() override = default;

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
    void subscribe(size_t index);
    void on_message_bookticker(const simdjson::dom::object& data, uint64_t recv_tsc, uint64_t recv_milli);
    int get_id() {return id++;}
private:
    std::atomic<int> id;
    HttpClient rest_;
    std::string rest_host_{};
    std::string pairs_info_path_{};
    std::string funding_fee_path_{};

    ConnectData wss_infos_;
    using WebSocketClient = WssClient<PhemexMarketData>;
    std::vector<std::shared_ptr<WebSocketClient>> wss_connections_;
    std::vector<std::vector<std::string>> stream_params_;
};
} // namespace infra
