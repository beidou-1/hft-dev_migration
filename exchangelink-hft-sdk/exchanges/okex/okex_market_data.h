#pragma once
#include "common/interface.h"
#include "common/json.h"
#include "network/rest_client.h"
#include "network/wss_client.h"

namespace infra {
class OkxMarketData : public IExchangeMarketData, public WssHandler {
public:
    OkxMarketData(net::io_context& ioc, ssl::context& ssl_ctx, const AccountSecret& sec, APIConfig config)
        : IExchangeMarketData(ioc, ssl_ctx, sec, config), rest_(ioc_, ssl_ctx_) {}
    ~OkxMarketData() override = default;

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

    void on_message_depthall(const simdjson::dom::object& data, std::string_view symbol);

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
