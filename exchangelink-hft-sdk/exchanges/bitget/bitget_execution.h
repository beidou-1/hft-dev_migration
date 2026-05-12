#pragma once
#include "bitget_utils.h"
#include "network/websocket.h"

namespace infra {
class FormatJson {
private:
    char* buf_{nullptr}; // 指向发送缓存区
    size_t cur_{0};

public:
    FormatJson() {}

    void set_buf(char* buf) {
        buf_ = buf;
        cur_ = 0;
    }

    void append(std::string_view value) {
        memcpy(buf_ + cur_, value.data(), value.size());
        cur_ += value.size();
    }

    void append(int64_t value) {
#if __GNUC__ >= 13
        auto [ptr, ec] = std::to_chars(buf_ + cur_, buf_ + cur_ + 32, value);
        cur_ = ptr - buf_;
#else
        int len = snprintf(buf_ + cur_, 32, "%ld", value);
        cur_ += len;
#endif
    }

    void append(double value) {
#if __GNUC__ >= 13
        auto [ptr, ec] = std::to_chars(buf_ + cur_, buf_ + cur_ + 32, value, std::chars_format::fixed, 10);
        cur_ = ptr - buf_;
#else
        int len = snprintf(buf_ + cur_, 32, "%.10f", value);
        cur_ += len;
#endif
    }

    size_t get_size() const { return cur_; }
    std::string_view get_json() const { return std::string_view(buf_, cur_); }
};

class BitgetExecution : public IExchangeExecution, public WssHandler {
public:
    BitgetExecution(net::io_context& ioc, ssl::context& ssl_ctx, const AccountSecret& sec, APIConfig config)
        : IExchangeExecution(ioc, ssl_ctx, sec, config), client_(ioc_, ssl_ctx_), wss_stream_(ioc_, ssl_ctx_, *this),
          wss_trade_(ioc_, ssl_ctx_, *this) {}
    ~BitgetExecution() override = default;

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
    void keep_ws_connection_alive(size_t index);

    void on_message_stream(const simdjson::dom::element& doc);
    void process_ws_response(std::string_view id, SpOrder rtn_order);

private:
    HttpClient client_;
    std::string rest_host_{};
    std::string place_order_path_{};
    std::string query_order_path_{};
    std::string cancel_order_path_{};

    using WebSocketClient = WssClient<BitgetExecution>;

    ConnectData wss_config_;
    WebSocketClient wss_stream_;

    ConnectData wss_trade_config_;
    WebSocketClient wss_trade_; // 用于下单&撤单

    struct RtnOrderCache {
        Symbol pair;
        ClientOrderId client_oid;
        OrderCallback cb;
    };
    std::unordered_map<int64_t, RtnOrderCache> ws_request_cache_;

    // 加速请求构造
    FormatJson json_cache_;

    static constexpr std::string_view FIXED_FIELD =
        R"({"op":"trade","topic":"place-order","category":"usdt-futures","args":[{"timeInForce":"ioc","orderType":)";
    static constexpr std::string_view CID_FIELD = R"("clientOid":")";
    static constexpr std::string_view PAIR_FIELD = R"(","symbol":")";
    static constexpr std::string_view QTY_FIELD = R"(","qty":")";
    static constexpr std::string_view PRICE_FIELD = R"(","price":")";
    static constexpr std::string_view UID_FIELD = R"("}],"id":")";
    static constexpr std::string_view END_FIELD = R"("})";

    static constexpr std::array<const char*, 8> filed_array = {
        R"("limit","side":"buy","reduceOnly":"NO",)",   R"("limit","side":"buy","reduceOnly":"YES",)",
        R"("limit","side":"sell","reduceOnly":"NO",)",  R"("limit","side":"sell","reduceOnly":"YES",)",
        R"("market","side":"buy","reduceOnly":"NO",)",  R"("market","side":"buy","reduceOnly":"YES",)",
        R"("market","side":"sell","reduceOnly":"NO",)", R"("market","side":"sell","reduceOnly":"YES",)"};
};
} // namespace infra