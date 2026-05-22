#pragma once
#include "kucoin_utils.h"

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
    /* classic */
    std::string get_public_token();
    void subscribe_classic(size_t index);
    void on_message_classic_bookticker(const simdjson::dom::object& data, uint64_t recv_tsc, uint64_t recv_milli);
    void on_message_classic_orderbook(const simdjson::dom::object& data, const Symbol& symbol, uint64_t recv_tsc, uint64_t recv_milli);
    bool subscribe_classic_orderbook(const Symbols& symbols, unsigned int depth, OrderbookCallback cb);
    Action on_classic_connect(Wss* ws);
    Action on_classic_message(Wss* ws, std::string_view msg);
    void fetch_classic_pairs_info(ExPairInfoCallback cb);
    void fetch_classic_funding_fee(const Symbol& symbol, FundingFeeCallback cb);

    /* unified */
    void subscribe_unified(size_t index);
    void on_message_unified_bookticker(const simdjson::dom::object& data, uint64_t recv_tsc, uint64_t recv_milli);
    void on_message_unified_orderbook(const simdjson::dom::object& data, uint64_t recv_tsc, uint64_t recv_milli);
    bool subscribe_unified_orderbook(const Symbols& symbols, unsigned int depth, OrderbookCallback cb);
    Action on_unified_connect(Wss* ws);
    Action on_unified_message(Wss* ws, std::string_view msg);
    void fetch_unified_pairs_info(ExPairInfoCallback cb);
    void fetch_unified_funding_fee(const Symbol& symbol, FundingFeeCallback cb);

private:
    HttpClient rest_;
    std::string rest_host_{};
    std::string pairs_info_path_{};
    std::string funding_fee_path_{};

    ConnectData wss_infos_;
    using WebSocketClient = WssClient<BitunixExecution>;
    std::vector<std::shared_ptr<WebSocketClient>> wss_connections_;
    std::vector<std::string> stream_params_;
};
} // namespace infra
