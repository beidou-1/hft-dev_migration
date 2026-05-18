#pragma once
#include "okex_utils.h"
#include "network/websocket.h"

namespace infra {
class OkxExecution : public IExchangeExecution, public WssHandler {
public:
    OkxExecution(net::io_context& ioc, ssl::context& ssl_ctx, const AccountSecret& sec, APIConfig config)
        : IExchangeExecution(ioc, ssl_ctx, sec, config), rest_(ioc_, ssl_ctx_), wss_stream_(ioc_, ssl_ctx_, *this),
          wss_api_(ioc_, ssl_ctx_, *this) {}
    ~OkxExecution() override = default;

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
    void login(int index);
    void send_http_request(const HttpRequestBody& req, SpOrder order, OrderCallback cb, std::string_view name);

    using WebSocketClient = WssClient<OkxExecution>;
    bool send_ws_request(WebSocketClient& client, const std::string& content, const std::string& name);
    inline void keep_ws_connection_alive(WebSocketClient& client) {
        client.start_ping_pong("ping", 25); // 心跳检测时间为25秒
    }

private:
    HttpClient rest_;
    std::string rest_host_{};
    std::string query_order_path_{};
    std::string place_order_path_{};
    std::string cancel_order_path_{};

    ConnectData wss_config_;
    WebSocketClient wss_stream_;

    ConnectData wss_api_config_;
    WebSocketClient wss_api_; // 用于下单&撤单
    UMClientIdOderCache cache_;
    std::unordered_map<std::string, std::pair<SpOrder, OrderCallback>> ws_request_cache_;
};
} // namespace infra
