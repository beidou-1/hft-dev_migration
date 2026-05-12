#pragma once

#include "shm/shm_types.h"

namespace hft {

// ── Secrets ────────────────────────────────────────────
struct KVPair {
    char key[LEN_64];
    char val[LEN_64];
};

struct Secrets {
    char     api_key[LEN_128];
    char     api_secret[LEN_128];
    char     api_phrase[LEN_128];
    char     wallet_address[LEN_128];

    uint32_t custom_info_count;
    uint32_t _pad;
    KVPair   custom_info[MAX_CUSTOM_INFO];
};

// ── 限频配置 ─────────────────────────────────────────────────
struct RateLimit {
    uint32_t place_time;
    uint32_t place_num;
};

/* ── 交易控制参数 ──────────────────────────────────────────────
交易控制由 ctrl_mode 和 resume_seq 两个字段协同实现。

ctrl_mode 语义：
  0 = 正常，允许开仓和平仓
  1 = 停止开仓（多空均不允许开新仓，只允许平仓）
  2 = StopOpenLong（禁止开多，允许开空和平仓）
  3 = StopOpenShort（禁止开空，允许开多和平仓）
  4 = 停止交易，开仓和平仓均禁止

引擎本地维护镜像变量（每次 poll_config 时从 ctrl_mode 同步）：
  is_cancel_trading_ = (ctrl_mode == 4)
  is_stop_open_      = (ctrl_mode >= 1)
  is_stop_open_long_ = (ctrl_mode == 1 || ctrl_mode == 2 || ctrl_mode == 4)
  is_stop_open_short_= (ctrl_mode == 1 || ctrl_mode == 3 || ctrl_mode == 4)

对冲失败流程：
  Step 1 - 引擎检测到对冲失败：
    · 置 is_hedge_failed_ = true
    · 记录 hedge_fail_seq_ = 当前 resume_seq（快照）
    · 置 is_cancel_trading_ / is_stop_open_ = true（停止交易）
    · 推送 HedgeFailEvent 到 shm 通知管理系统
  Step 2 - 管理系统收到事件，检查敞口确认正常后：
    · 将 ctrl_mode 置为目标状态（通常为 0）
    · 将 resume_seq +1（明确的恢复确认信号）
  Step 3 - 引擎 poll_config 检测到 resume_seq > hedge_fail_seq_：
    · 清除 is_hedge_failed_
    · 重置 is_cancel_trading_ / is_stop_open_ 为 ctrl_mode 对应值
    · 恢复交易
关键点：仅凭 ctrl_mode 不足以触发恢复，必须同时满足 resume_seq 递增。
这样即使 config 在管理系统处理前被刷新（ctrl_mode 仍为 0），引擎也不会误恢复，避免了时序竞争。
*/

struct Params {
    uint32_t  orderbook_timeout_milli;
    uint32_t  resume_seq;             // 管理系统每次确认恢复时 +1（单调递增）
    RateLimit rate_limits[2];         // 0: master, 1: slave
    uint8_t   ctrl_mode;              // 0=正常 1=停止开仓 2=StopOpenLong 3=StopOpenShort 4=停止交易
    uint8_t   _pad[7];
};

// ── Spreads ──────────────────────────────────────────────
struct Spreads {
    char     symbol[LEN_32];

    double   fragment;
    double   fragment_min;
    double   position;
    double   taker_scale;

    double   long_open;
    double   long_close;
    double   short_open;
    double   short_close;
};

// ── 共享内存根结构 ────────────────────────────────────────────
// seqlock (SPSC)
// 写进程：version +1(奇数=写中) → 写数据 → version +1(偶数=完成)
// 读进程：v1=version(偶数则继续) → 读数据 → v2=version(v1==v2则有效)
struct StrategyConfig {
    std::atomic<uint32_t> version{0};
    uint32_t spread_count;       
    char     id[LEN_128];     
    Secrets  secrets[2];         // 0: master, 1: slave
    Params   params;
    Spreads  spreads[MAX_SPREADS];
};

static_assert(sizeof(Secrets) % 8 == 0, "Secrets must be 8-byte aligned");
static_assert(sizeof(Spreads)   % 8 == 0, "Spreads must be 8-byte aligned");
static_assert(sizeof(StrategyConfig)  % 8 == 0, "StrategyConfig must be 8-byte aligned");

} // namespace hft
