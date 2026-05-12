#pragma once
#include "gate_utils.h"

namespace infra {
class GateAccount : public IExchangeAccount {
public:
    GateAccount(net::io_context& ioc, ssl::context& ssl_ctx, const AccountSecret& sec, APIConfig config)
        : IExchangeAccount(ioc, ssl_ctx, sec, config), client_(ioc_, ssl_ctx_) {}
    ~GateAccount() override = default;

    bool initialize() override;
    void shutdown() override {};

    void get_margin_ratio(MarginRatioCallback cb) override;
    void get_balance(const Currency& currency, BalanceCallback cb) override;
    void get_position(const Symbol& symbol, PositionCallback cb) override;
    void set_leverage(const Symbol& symbol, unsigned int leverage, MarginMode mode, LeverageCallback cb) override;

private:
    HttpClient client_;
    std::string rest_host_{};
    std::string balance_path_{};
    std::string position_path_{};
    std::string leverage_path_{};
};
} // namespace infra
