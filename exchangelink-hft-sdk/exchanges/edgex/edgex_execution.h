#pragma once
#include "edgex_utils.h"
#include "network/websocket.h"

namespace infra {
class EdgexExecution : public IExchangeExecution, public WssHandler {
public:
    EdgexExecution(net::io_context& ioc, ssl::context& ssl_ctx, const AccountSecret& sec, APIConfig config)
        : IExchangeExecution(ioc, ssl_ctx, sec, config), rest_(ioc_, ssl_ctx_), wss_stream_(ioc_, ssl_ctx_, *this) {}
    ~EdgexExecution() override = default;

    bool initialize() override;
    void shutdown() override;

    void query_order(const SpOrder order, OrderCallback cb) override;
    void place_order(const SpOrder order, OrderCallback cb) override;
    void cancel_order(const SpOrder order, OrderCallback cb) override;

    bool subscribe_order(OrderCallback cb) override;
    void unsubscribe_order() override;

public:
    Action on_connect(Wss* ws) override;
    Action on_ping(Wss* ws, std::string_view payload) override;
    Action on_pong(Wss* ws, std::string_view payload) override;
    void on_close(Wss* ws) override;
    void on_error(Wss* ws, std::string_view err) override;
    Action on_message(Wss* ws, std::string_view msg) override;
    
    void sign_ws(websocket::request_type& req);
private:
    void login();
    bool convert_place_order(SpOrder order, OrderCallback cb, std::string& payload, std::string& sorted_payload);
    void send_http_request(const HttpRequestBody& req, SpOrder order, OrderCallback cb, std::string_view name);
    double get_maker_price(const Symbol& symbol);
private:
    HttpClient rest_;
    std::string rest_host_{};
    std::string query_order_path_{};
    std::string place_order_path_{};
    std::string cancel_order_path_{};

    ConnectData wss_config_;
    using WebSocketClient = WssClient<EdgexExecution>;
    WebSocketClient wss_stream_;
    std::unordered_map<Symbol, double> tickers;
    std::unordered_map<std::string, std::pair<SpOrder, OrderCallback>> ws_request_cache_;
};
} // namespace infra
