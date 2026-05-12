/**
 * @file orderbook.h
 * @brief 订单簿数据结构
 * @details 实现订单簿的存储和管理，支持全量和增量更新
 */

#pragma once

#include <memory>
#include <string>
#include <iostream>
#include <iomanip>
#include "types.h"

namespace infra {
class OrderBook {
public:
    OrderBook(const std::string& pair, int64_t update_milli, const double& ask_price, const double& ask_vol,
              const double& bid_price, const double& bid_vol)
        : pair(pair), update_milli(update_milli), ask_price(ask_price), ask_qty(ask_vol), bid_price(bid_price),
          bid_qty(bid_vol) {}

    Symbol pair;                ///< 交易对
    Timestamp update_milli = 0; ///< 交易所更新时间戳(ms)
    Timestamp recv_milli = 0;   ///< 系统接收行情的时间戳(ms)
    Timestamp recv_tsc = 0;     ///< 系统接收到行情的tsc
    Timestamp parsed_tsc = 0;   ///< 解析完行情的tsc

    double ask_price;
    double ask_qty;
    double bid_price;
    double bid_qty;

    void print() const {
        std::cout << "\n===== " << pair << " (t=" << update_milli << ") =====\n";
        std::cout << std::fixed << std::setprecision(5);
        std::cout << "[ASKS]\n";
        std::cout << ask_price << "\t" << ask_qty << "\n";
        std::cout << "-----------------------------\n[BIDS]\n";
        std::cout << bid_price << "\t" << bid_qty << "\n";
        std::cout << "=============================\n";
    }
}; // class OrderBook

using SpOrderBook = std::shared_ptr<OrderBook>;

} // namespace infra
