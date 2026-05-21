#pragma once
#include "hyperliquid_utils.h"

namespace infra {
class HyperliquidExecution : public IExchangeExecution, public WssHandler {
public:
    HyperliquidExecution(net::io_context& ioc, ssl::context& ssl_ctx, const AccountSecret& sec, APIConfig config)
        : IExchangeExecution(ioc, ssl_ctx, sec, config), rest_(ioc_, ssl_ctx_), wss_stream_(ioc_, ssl_ctx_, *this) {}
    ~HyperliquidExecution() override = default;

    bool initialize() override;
    void shutdown() override;

    void query_order(const SpOrder order, OrderCallback cb) override;

    bool subscribe_order(OrderCallback cb) override;
    void unsubscribe_order() override;

    void place_order(const SpOrder order, OrderCallback cb) override;
    void cancel_order(const SpOrder order, OrderCallback cb) override;

public:
    Action on_connect(Wss* ws) override;
    Action on_ping(Wss* ws, std::string_view payload) override;
    Action on_pong(Wss* ws, std::string_view payload) override;
    void on_close(Wss* ws) override;
    void on_error(Wss* ws, std::string_view err) override;
    Action on_message(Wss* ws, std::string_view msg) override;

private:
    void login();
    void keep_ws_connection_alive();
    unsigned long generate_req_id() { return ++req_id_; }

    bool convert_place_order(SpOrder order, OrderCallback cb, std::string& payload);
    bool convert_cancel_order(SpOrder order, OrderCallback cb, std::string& payload);
    void send_http_request(const HttpRequestBody& req, SpOrder order, OrderCallback cb, std::string_view name);
    void send_ws_request(std::string&& content);

private:
    HttpClient rest_;
    std::string rest_host_{};
    std::string query_order_path_{};
    std::string place_order_path_{};
    std::string cancel_order_path_{};
    unsigned long req_id_{0};

    ConnectData wss_config_;
    WebSocketClient wss_stream_;

    std::unordered_map<unsigned long, std::pair<SpOrder, OrderCallback>> ws_request_cache_;
};
} // namespace infra
