#pragma once
#include "bybit_utils.h"
#include "network/websocket.h"
#include "common/interface.h"
#include "exchanges/signature.h"
namespace infra {
class BybitExecution : public IExchangeExecution, public WssHandler {
public:
    BybitExecution(net::io_context& ioc, ssl::context& ssl_ctx, const AccountSecret& sec, APIConfig config)
        : IExchangeExecution(ioc, ssl_ctx, sec, config), rest_(ioc_, ssl_ctx_), wss_stream_(ioc_, ssl_ctx_, *this),
          wss_trade_(ioc_, ssl_ctx_, *this) {}
    ~BybitExecution() override = default;

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
    void login(size_t index);
    void keep_ws_connection_alive(size_t index);
    unsigned long generate_req_id() { return ++req_id_; }

    bool convert_cancel_order(SpOrder order, OrderCallback cb, std::string& res);

    void send_http_request(const HttpRequestBody& req, SpOrder order, OrderCallback cb, std::string_view name);
    using WebSocketClient = WssClient<BybitExecution>;
    bool send_ws_request(WebSocketClient& client, const std::string& content, const std::string& name);

private:
    HttpClient rest_;
    std::string rest_host_{};
    std::string query_order_path_{};
    std::string place_order_path_{};
    std::string cancel_order_path_{};
    std::string category_;
    unsigned long req_id_{0};

    // using WebSocketClient = WssClient<BybitExecution>;

    ConnectData wss_config_;
    WebSocketClient wss_stream_;

    ConnectData wss_trade_config_;
    WebSocketClient wss_trade_; // 用于下单&撤单
    std::unordered_map<unsigned long, std::pair<SpOrder, OrderCallback>> ws_request_cache_;
};
} // namespace infra