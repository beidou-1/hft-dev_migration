#pragma once

#include <string>
#include "shm/shm_spreads.h"
#include "shm/shm_data.h"

namespace hft {

class ShmManager {
public:
    explicit ShmManager(const std::string& name) : name_(name) {}
    ~ShmManager() { detach(); }

    ShmManager(const ShmManager&)            = delete;
    ShmManager& operator=(const ShmManager&) = delete;

    bool create();
    bool attach();
    void detach();

    bool valid() const { return config_ != nullptr && data_ != nullptr; }

    /// 获取 StrategyConfig 指针（管理端写端专用，attach 模式下返回 nullptr）
    StrategyConfig* config() noexcept { return owner_ ? config_ : nullptr; }

    // ── 写端：StrategyConfig ──────────────────────────────────
    void write_config(StrategyConfig& src);

    // ── 读端：StrategyConfig ──────────────────────────────────
    // 有新版本则写入 snap 并返回 true，无更新返回 false
    bool poll_config(StrategyConfig& snap);

    // ── 写端：TradeData ───────────────────────────────────────
    bool push_slippage(const SlipPageEvent& ev);
    bool push_hedge_fail(const HedgeFailEvent& ev);
    bool push_latency(const LatencyEvent& ev);

    // ── 读端：TradeData ───────────────────────────────────────
    bool pop_slippage(SlipPageEvent& ev);
    bool pop_hedge_fail(HedgeFailEvent& ev);
    bool pop_latency(LatencyEvent& ev);

private:
    std::string     name_;
    int             fd_config_           = -1;
    int             fd_data_             = -1;
    StrategyConfig* config_              = nullptr;
    TradeData*      data_                = nullptr;
    bool            owner_               = false;
    uint32_t        last_config_version_ = 0;
};

} // namespace hft
