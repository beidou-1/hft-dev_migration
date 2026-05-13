#pragma once
#include "common/interface.h"
#include "network/rest_client.h"

namespace infra {
class OkxAccount : public IExchangeAccount {
public:
    OkxAccount(net::io_context& ioc, ssl::context& ssl_ctx, const AccountSecret& sec, APIConfig config)
        : IExchangeAccount(ioc, ssl_ctx, sec, config), rest_(ioc_, ssl_ctx_) {}
    ~OkxAccount() override = default;

    bool initialize() override;
    void shutdown() override {};

    void get_balance(const Currency& currency, BalanceCallback cb) override;
    void get_position(const Symbol& symbol, PositionCallback cb) override;

    bool set_leverage(const Symbol& symbol, unsigned int leverage, MarginMode mode) override;

private:
    bool send_http_request_sync(HttpRequestBody& req, const std::string& function_name);

private:
    HttpClient rest_;
    std::string rest_host_{};
    std::string balance_path_{};
    std::string position_path_{};
    std::string leverage_path_{};
    std::string margin_mode_path_{};
    std::string position_mode_path_{};
};
} // namespace infra
