/**
 * @file client.h
 * @brief 交易所客户端封装类
 * @details 提供统一的交易所客户端接口，封装账户、行情和交易执行功能
 */

#pragma once

#include <memory>
#include <functional>
#include "interface.h"

namespace infra {

/**
 * @class ExchangeClient
 * @brief 交易所客户端统一封装类
 * @details 整合账户管理、行情数据和交易执行三大模块，提供统一的访问接口
 */
class ExchangeClient {
public:
    /**
     * @brief 构造函数
     * @param account 账户管理模块
     * @param market_data 行情数据模块
     * @param execution 交易执行模块
     */
    ExchangeClient(UpAccount account, UpMarketData market_data, UpExecution execution)
        : account_(std::move(account)), market_data_(std::move(market_data)), execution_(std::move(execution)) {}

    /**
     * @brief 析构函数
     */
    ~ExchangeClient() { shutdown(); }

    /**
     * @brief 初始化所有模块
     * @param enable_all 值为true时初始化所有模块，为false时只初始化行情模块
     * @return 初始化成功返回true，否则返回false
     */
    bool initialize(bool enable_all = true) {
        bool ok = true;
        if (market_data_)
            ok &= market_data_->initialize();
        if (enable_all && execution_)
            ok &= execution_->initialize();
        if (enable_all && account_)
            ok &= account_->initialize();
        return ok;
    }

    /**
     * @brief 关闭所有模块
     */
    void shutdown() {
        if (account_)
            account_->shutdown();
        if (execution_)
            execution_->shutdown();
        if (market_data_)
            market_data_->shutdown();
    }

    // ==================== MarketData ====================
    /**
     * @brief 订阅订单簿数据
     * @param syms 交易对列表，值为空时订阅全量行情
     * @param depth 订阅档位
     * @param cb 订单簿回调函数
     * @return 订阅成功返回true，否则返回false
     */
    bool subscribe_orderbook(const Symbols& syms, unsigned int depth, OrderbookCallback cb) {
        return market_data_->subscribe_orderbook(syms, depth, std::move(cb));
    }

    /**
     * @brief 取消订阅订单簿
     */
    void unsubscribe_orderbook() { market_data_->unsubscribe_orderbook(); }

    /**
     * @brief 获取交易对信息
     * @param cb 回调函数
     */
    void fetch_pairs_info(ExPairInfoCallback cb) { market_data_->fetch_pairs_info(std::move(cb)); }

    /**
     * @brief 获取资金费率
     * @param s 交易对符号
     * @param cb 回调函数
     */
    void fetch_funding_fee(const Symbol& s, FundingFeeCallback cb) {
        market_data_->fetch_funding_fee(s, std::move(cb));
    }

    /**
     * @brief 获取指定交易对的订单簿快照
     * @param s 交易对符号
     * @return 订单簿智能指针
     */
    SpOrderBook get_orderbook(const Symbol& s) const { return market_data_->get_orderbook(s); }

    // ==================== Execution ====================
    /**
     * @brief 订阅订单更新
     * @param cb 订单回调函数
     * @return 订阅成功返回true，否则返回false
     */
    bool subscribe_order(OrderCallback cb) { return execution_->subscribe_order(std::move(cb)); }

    /**
     * @brief 取消订阅订单更新
     */
    void unsubscribe_order() { execution_->unsubscribe_order(); }

    /**
     * @brief 通过WebSocket接口下单（部分交易所不支持）
     * @param o 订单对象
     * @param cb 回调函数
     */
    void place_order(const SpOrder& o, OrderCallback cb) { execution_->place_order(o, std::move(cb)); }

    /**
     * @brief 通过WebSocket接口撤单（部分交易所不支持）
     * @param o 订单对象
     * @param cb 回调函数
     */
    void cancel_order(const SpOrder& o, OrderCallback cb) { execution_->cancel_order(o, std::move(cb)); }

    /**
     * @brief 查询订单状态
     * @param o 订单对象
     * @param cb 回调函数
     */
    void query_order(const SpOrder& o, OrderCallback cb) { execution_->query_order(o, std::move(cb)); }

    // ==================== Account ====================
    /**
     * @brief 获取指定货币的余额
     * @param c 货币符号，值为空时返回所有资产信息，格式要求为大写，如USDT
     * @param cb 回调函数
     */
    void get_balance(const Currency& c, BalanceCallback cb) { account_->get_balance(c, std::move(cb)); }

    /**
     * @brief 获取指定交易对的持仓
     * @param s 交易对符号，值为空时返回所有
     * @param cb 回调函数
     */
    void get_position(const Symbol& s, PositionCallback cb) { account_->get_position(s, std::move(cb)); }

    /**
     * @brief 获取维持保证金率
     * @param cb 回调函数
     */
    void get_margin_ratio(MarginRatioCallback cb) { account_->get_margin_ratio(std::move(cb)); }

    /**
     * @brief 设置杠杆倍数
     * @param s 交易对符号
     * @param lev 杠杆倍数
     * @param m 保证金模式
     * @return 设置成功返回true，否则返回false
     */
    void set_leverage(const Symbol& s, unsigned int lev, MarginMode m, LeverageCallback cb) {
        account_->set_leverage(s, lev, m, std::move(cb));
    }

private:
    UpAccount account_;        ///< 账户管理模块
    UpMarketData market_data_; ///< 行情数据模块
    UpExecution execution_;    ///< 交易执行模块
};

/// 交易所客户端智能指针类型
using SpExchangeClient = std::shared_ptr<ExchangeClient>;

} // namespace infra
