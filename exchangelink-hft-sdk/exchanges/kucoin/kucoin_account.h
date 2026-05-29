#pragma once
#include "kucoin_utils.h"

namespace infra {
class KucoinAccount : public IExchangeAccount {
public:
    KucoinAccount(net::io_context& ioc, ssl::context& ssl_ctx, const AccountSecret& sec, APIConfig config)
        : IExchangeAccount(ioc, ssl_ctx, sec, config), rest_(ioc_, ssl_ctx_) {}
    ~KucoinAccount() override = default;

    bool initialize() override;
    void shutdown() override {};

    void get_balance(const Currency& currency, BalanceCallback cb) override;
    void get_position(const Symbol& symbol, PositionCallback cb) override;

    void set_leverage(const Symbol& symbol, unsigned int leverage, MarginMode mode, LeverageCallback cb) override;
    void get_margin_ratio(MarginRatioCallback cb) override;

private:
    bool send_http_request_sync(const HttpRequestBody& req, std::string_view name);
    void get_classic_balance(const Currency& currency, BalanceCallback cb);
    void get_unified_balance(const Currency& currency, BalanceCallback cb);

private:
    HttpClient rest_;
    std::string rest_host_{};
    std::string balance_path_{};
    std::string position_path_{};
    std::string leverage_path_{};
};
} // namespace infra
