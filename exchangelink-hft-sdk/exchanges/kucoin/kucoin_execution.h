#pragma once
#include "kucoin_utils.h"
#include "network/websocket.h"

namespace infra {
class KucoinExecution : public IExchangeExecution, public WssHandler {
public:
    KucoinExecution(net::io_context& ioc, ssl::context& ssl_ctx, const AccountSecret& sec, APIConfig config)
        : IExchangeExecution(ioc, ssl_ctx, sec, config), rest_(ioc_, ssl_ctx_), wss_stream_(ioc_, ssl_ctx_, *this),
          wss_trade_(ioc_, ssl_ctx_, *this) {}
    ~KucoinExecution() override = default;

    bool initialize() override;
    void shutdown() override;

    void query_order(const SpOrder& order, OrderCallback cb) override;

    bool subscribe_order(OrderCallback cb) override;
    void unsubscribe_order() override;

    void place_order(const SpOrder& order, OrderCallback cb) override;
    void cancel_order(const SpOrder& order, OrderCallback cb) override;

public:
    Action on_connect(Wss* ws) override;
    Action on_ping(Wss* ws, std::string_view payload) override;
    Action on_pong(Wss* ws, std::string_view payload) override;
    void on_close(Wss* ws) override;
    void on_error(Wss* ws, std::string_view err) override;
    Action on_message(Wss* ws, std::string_view msg) override;

private:
    using WebSocketClient = WssClient<KucoinExecution>;
    bool login(std::string_view msg);
    unsigned long generate_req_id() { return ++req_id_; }

    void send_http_request(const HttpRequestBody& req, SpOrder order, OrderCallback cb, std::string_view name);
    bool send_ws_request(WebSocketClient& client, const std::string& content, const std::string& name);
    std::string get_private_token();
    
    void query_classic_order(const SpOrder& order, OrderCallback cb);
    void query_unified_order(const SpOrder& order, OrderCallback cb);
    void place_classic_order_ws(const SpOrder& order, OrderCallback cb);
    void place_unified_order_ws(const SpOrder& order, OrderCallback cb);
    void cancel_classic_order_ws(const SpOrder& order, OrderCallback cb);
    void cancel_unified_order_ws(const SpOrder& order, OrderCallback cb);

    Action on_classic_message(Wss* ws, std::string_view msg);
    Action on_unified_message(Wss* ws, std::string_view msg);
    void keep_private_ws_connection_alive(WebSocketClient& client);

    void get_account_mode();
    inline void keep_ws_connection_alive(WebSocketClient& client) {
        std::string msg = fmt::format(R"({{"id":"{}","type":"ping"}})", time_get_now_micro());
        client.start_ping_pong(msg, 15); // 心跳检测时间为20秒
    }

private:
    HttpClient rest_;
    std::string rest_host_{};
    std::string order_path_{};
    std::string cancel_order_path_{};
    std::string query_order_path_{};
    unsigned long req_id_{0};

    ConnectData wss_config_;
    WebSocketClient wss_stream_;

    ConnectData wss_trade_config_;
    WebSocketClient wss_trade_; // 用于下单&撤单
    std::unordered_map<unsigned long, std::pair<SpOrder, OrderCallback>> ws_request_cache_;
};
} // namespace infra
