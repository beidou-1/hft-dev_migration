#pragma once
#include "edgex_utils.h"

namespace infra {
class EdgexAccount : public IExchangeAccount {
public:
    EdgexAccount(net::io_context& ioc, ssl::context& ssl_ctx, const AccountSecret& sec, APIConfig config)
        : IExchangeAccount(ioc, ssl_ctx, sec, config), rest_(ioc_, ssl_ctx_) {}
    ~EdgexAccount() override = default;

    bool initialize() override;
    void shutdown() override{};

    UMCurrencyBalance get_balance(const Currency& currency) override;
    UMSymbolPosition get_position(const Symbol& symbol) override;

    void get_balance(const Currency& currency, BalanceCallback cb) override;
    void get_position(const Symbol& symbol, PositionCallback cb) override;

    bool set_leverage(const Symbol& symbol, unsigned int leverage, MarginMode mode) override;
    bool set_margin_mode(const Symbol& symbol, MarginMode mode) override;
    bool set_position_mode(PositionMode mode) override;

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
