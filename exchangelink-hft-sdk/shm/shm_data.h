#pragma once

#include <cstdint>
#include "shm/shm_types.h"

namespace hft {

// ── 滑点事件 ───────────────────────────────
struct SlipPageEvent {
    int64_t update_milli;
    char    pair[LEN_32];
    char    m_cid[LEN_64];
    char    s_cid[LEN_64];
    char    s_oid[LEN_64];
    char    s_side[LEN_32];           // "OpenLong" / "OpenShort" / ...

    double  deploy_spread;
    
    double  m_ob_price;
    double  m_order_price;
    double  m_avg_price;

    double  s_exp_price;                 
    double  s_ask0_price;               
    double  s_bid0_price;                
    double  s_avg_price;                 
    double  s_cum_deal;
    // 读端可由以下字段计算：
    // is_buy = (s_side == "OpenLong" || s_side == "CloseShort")
    // slip_page       = is_buy ? (s_avg-s_exp)/s_exp         : (s_exp-s_avg)/s_exp
    // real_spread     = is_buy ? m_avg/s_avg                 : s_avg/m_avg
    // orderbook_drift = is_buy ? (s_avg-s_ask0)/s_ask0       : (s_avg-s_bid0)/s_bid0
};

static_assert(sizeof(SlipPageEvent) % 8 == 0, "SlipPageEvent must be 8-byte aligned");

// ── 对冲失败事件 ─────────────────────────────────────────
struct HedgeFailEvent {
    int64_t milli;
    char    pair[LEN_32];
    char    s_cid[LEN_64];
    char    s_oid[LEN_64];
    char    s_side[LEN_32];
    uint8_t mode;               // 0=close_only, 1=cancel_trading
    uint8_t _pad[7];
    char    sys_msg[LEN_128];
    char    ex_msg[LEN_128];
};

static_assert(sizeof(HedgeFailEvent) % 8 == 0, "HedgeFailEvent must be 8-byte aligned");

// ── 全链路延迟事件 ────────────────────────────────────────────
struct LatencyEvent {
    int64_t  milli;                 ///< 事件时间戳
    char     m_cid[LEN_64];         ///< master 订单 ID

    // master/slave 行情
    int64_t  m_ob_ex_milli;         ///< master 行情交易所时间
    int64_t  m_ob_recv_milli;       ///< master 行情本地接收时间
    uint64_t m_ob_recv_tsc;         ///< master 行情接收 tsc
    uint64_t m_ob_parsed_tsc;       ///< master 行情解析 tsc

    int64_t  s_ob_ex_milli;         ///< slave 行情交易所时间
    int64_t  s_ob_recv_milli;       ///< slave 行情本地接收时间
    uint64_t s_ob_recv_tsc;         ///< slave 行情接收 tsc
    uint64_t s_ob_parsed_tsc;       ///< slave 行情解析 tsc

    // master 订单
    uint64_t m_send_tsc;            ///< master 发单 tsc
    uint64_t m_sent_tsc;            ///< master 发单后 tsc
    uint64_t m_ack_tsc;             ///< master 首次回报 tsc
    uint64_t m_fill_tsc;            ///< master 成交 tsc

    // slave 订单
    uint64_t s_send_tsc;            ///< slave 发单 tsc
    uint64_t s_sent_tsc;            ///< slave 发单后 tsc
    uint64_t s_ack_tsc;             ///< slave 首次回报 tsc
    uint64_t s_fill_tsc;            ///< slave 成交 tsc

    uint64_t cpu_hz;                ///< CPU 主频 (Hz)，用于 tsc 换算纳秒
};

static_assert(sizeof(LatencyEvent) % 8 == 0, "LatencyEvent must be 8-byte aligned");

// ── 共享内存数据区 ────────────────────────────────────────────
// 策略进程（写端）push，外部进程（读端）pop
struct TradeData {
    SpscQueue<SlipPageEvent,  512> slippages;
    SpscQueue<HedgeFailEvent, 256> hedge_fails;
    SpscQueue<LatencyEvent,   4096> latencies;
};

} // namespace hft
