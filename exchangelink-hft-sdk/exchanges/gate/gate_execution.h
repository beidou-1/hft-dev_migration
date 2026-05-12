#pragma once
#include "gate_utils.h"
#include "network/websocket.h"

namespace infra {
class GateExecution : public IExchangeExecution, public WssHandler {
public:
    GateExecution(net::io_context& ioc, ssl::context& ssl_ctx, const AccountSecret& sec, APIConfig config)
        : IExchangeExecution(ioc, ssl_ctx, sec, config), client_(ioc_, ssl_ctx_), wss_stream_(ioc_, ssl_ctx_, *this),
          wss_trade_(ioc_, ssl_ctx_, *this) {}
    ~GateExecution() override = default;

    bool initialize() override;
    void shutdown() override;

    bool subscribe_order(OrderCallback cb) override;
    void unsubscribe_order() override;

    void place_order(const SpOrder order, OrderCallback cb) override;
    void cancel_order(const SpOrder order, OrderCallback cb) override;
    void query_order(const SpOrder order, OrderCallback cb) override;

public:
    Action on_connect(Wss* ws) override;
    Action on_ping(Wss* ws, std::string_view payload) override;
    Action on_pong(Wss* ws, std::string_view payload) override;
    void on_close(Wss* ws) override;
    void on_error(Wss* ws, std::string_view err) override;
    Action on_message(Wss* ws, std::string_view msg) override;

private:
    void login(size_t index);
    void add_header(websocket::request_type& req);

    using WebSocketClient = WssClient<GateExecution>;
    void send_http_request(const HttpRequestBody& req, SpOrder order, OrderCallback cb, std::string_view name);
    bool send_ws_request(WebSocketClient& client, const std::string& content, const std::string& name);
    unsigned long generate_req_id() { return ++req_id_; }

private:
    HttpClient client_;
    std::string rest_host_{};
    std::string order_path_{};
    unsigned long req_id_{0};

    ConnectData wss_config_;
    WebSocketClient wss_stream_;

    ConnectData wss_trade_config_;
    WebSocketClient wss_trade_; // 用于下单&撤单
    std::unordered_map<unsigned long, std::pair<SpOrder, OrderCallback>> ws_request_cache_;
};
} // namespace infra
