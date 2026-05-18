#pragma once
#include "toobit_utils.h"

namespace infra {
class ToobitExecution : public IExchangeExecution, public WssHandler {
public:
    ToobitExecution(net::io_context& ioc, ssl::context& ssl_ctx, const AccountSecret& sec, APIConfig config)
        : IExchangeExecution(ioc, ssl_ctx, sec, config), rest_(ioc_, ssl_ctx_), wss_stream_(ioc_, ssl_ctx_, *this) {}
    ~ToobitExecution() override = default;

    bool initialize() override;
    void shutdown() override;

    void query_order(const SpOrder order, OrderCallback cb) override;
    void place_order_rest(const SpOrder order, OrderCallback cb) override;
    void cancel_order_rest(const SpOrder order, OrderCallback cb) override;

    bool subscribe_order(OrderCallback cb) override;
    void unsubscribe_order() override;

    void place_order_ws(const SpOrder order, OrderCallback cb) override;
    void cancel_order_ws(const SpOrder order, OrderCallback cb) override;

public:
    Action on_connect(Wss* ws) override;
    Action on_ping(Wss* ws, std::string_view payload) override;
    Action on_pong(Wss* ws, std::string_view payload) override;
    void on_close(Wss* ws) override;
    void on_error(Wss* ws, std::string_view err) override;
    Action on_message(Wss* ws, std::string_view msg) override;

private:
    void keep_ws_connection_alive();

    void get_listen_key();        // initialize时调用，异步版本
    bool get_listen_key_sync();   // subscribe_order时调用，同步版本
    void keep_listen_key_alive(); // listenKey有效期为60分钟, 每25分钟主动延长有效期
    
    bool convert_place_order(SpOrder order, OrderCallback cb, std::string& payload);
    void send_http_request(const HttpRequestBody& req, SpOrder order, OrderCallback cb, std::string_view name);

private:
    HttpClient rest_;
    std::string rest_host_{};
    std::string order_path_{};
    std::string listen_key_path_{};
    std::string listen_key_{};

    ConnectData wss_config_;
    WebSocketClient wss_stream_;
};
} // namespace infra
