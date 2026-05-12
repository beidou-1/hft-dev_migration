#pragma once
#include "hbg_utils.h"
#include "network/websocket.h"

namespace infra {
class HbgMarketData : public IExchangeMarketData, public WssHandler {
public:
    HbgMarketData(net::io_context& ioc, ssl::context& ssl_ctx, const AccountSecret& sec, APIConfig config)
        : IExchangeMarketData(ioc, ssl_ctx, sec, config), client_(ioc_, ssl_ctx_) {}
    ~HbgMarketData() override = default;

    bool initialize() override;
    void shutdown() override;

    bool subscribe_orderbook(const Symbols& symbols, unsigned int depth, OrderbookCallback cb) override;
    void unsubscribe_orderbook() override;

    void fetch_pairs_info(ExPairInfoCallback cb) override;
    void fetch_funding_fee(const Symbol& symbol, FundingFeeCallback cb) override;

    bool subscribe_bookticker(const Symbols& symbols, OrderbookCallback cb);
    void unsubscribe_bookticker();

public:
    Action on_connect(Wss* ws) override;
    Action on_ping(Wss* ws, std::string_view payload) override;
    Action on_pong(Wss* ws, std::string_view payload) override;
    void on_close(Wss* ws) override;
    void on_error(Wss* ws, std::string_view err) override;
    Action on_message(Wss* ws, std::string_view msg) override;

private:
    void subscribe(size_t index);
    void on_message_bookticker(const simdjson::dom::object& data, const std::string& channel, uint64_t recv_tsc, uint64_t recv_milli);
    unsigned long generate_req_id() { return ++req_id_; }
    using WebSocketClient = WssClient<HbgMarketData>;
    bool send_ws_request(const std::shared_ptr<WebSocketClient>& client, const std::string& content,
                         const std::string& name);
    Action on_ping_hbg(Wss* ws, int64_t ts);

private:
    HttpClient client_;
    std::string rest_host_{};
    std::string pairs_info_path_{};
    std::string funding_fee_path_{};
    unsigned long req_id_{0};

    ConnectData wss_infos_;
    
    std::vector<std::shared_ptr<WebSocketClient>> wss_connections_;
    std::vector<std::vector<std::string>> stream_params_;
};
} // namespace infra
