/**
 * @file interface.h
 * @brief 交易所接口定义
 * @details 定义账户、行情数据和交易执行的抽象接口
 */

#pragma once

#include <memory>
#include <string>
#include <vector>
#include <functional>
#include <unordered_map>
#include "orderbook.h"
#include "structs.h"

namespace boost::asio {
class io_context;
namespace ssl {
class context;
}
} // namespace boost::asio

namespace net = boost::asio;
namespace ssl = net::ssl;

namespace infra {
/// 交易对信息映射表类型
using UMSymbolExInfo = std::unordered_map<Symbol, SpExPairInfo>;
/// 订单缓存
using UMClientIdOderCache = std::unordered_map<ClientOrderId, SpOrder>;
/// 货币余额映射表类型
using UMCurrencyBalance = std::unordered_map<Currency, SpBalance>;
/// 交易对持仓映射表类型
using UMSymbolPosition = std::unordered_map<Symbol, SpPosition>;

/// 订单簿回调函数类型
using OrderbookCallback = std::function<void(SpOrderBook)>;
/// 交易对信息回调函数类型
using ExPairInfoCallback = std::function<void(Errno, const UMSymbolExInfo&)>;
/// 资金费率回调函数类型
using FundingFeeCallback = std::function<void(Errno, SpFundingFee)>;
/// 订单回调函数类型
using OrderCallback = std::function<void(Errno, SpOrder)>;
/// ws下单回调函数缓存
using UMClientIdCallBack = std::unordered_map<ClientOrderId, OrderCallback>;
/// 余额查询回调函数类型
using BalanceCallback = std::function<void(Errno, const UMCurrencyBalance&)>;
/// 持仓查询回调函数类型
using PositionCallback = std::function<void(Errno, const UMSymbolPosition&)>;
/// 保证金比率查询回调函数类型
using MarginRatioCallback = std::function<void(Errno, const double&)>;
/// 设置杠杆回调函数类型
using LeverageCallback = std::function<void(Errno)>;

/**
 * @class IExchangeMarketData
 * @brief 交易所行情数据接口
 * @details 定义行情数据相关操作的抽象接口，包括订单簿订阅、交易对信息获取等
 */
class IExchangeMarketData {
public:
    /**
     * @brief 构造函数
     * @param ioc IO上下文
     * @param ssl_ctx SSL上下文
     * @param sec 账户密钥
     * @param config API配置
     */
    IExchangeMarketData(net::io_context& ioc, ssl::context& ssl_ctx, const AccountSecret& sec, APIConfig config)
        : ioc_(ioc), ssl_ctx_(ssl_ctx), account_secret_(sec), base_config_(config) {}
    virtual ~IExchangeMarketData() = default;

    /**
     * @brief 初始化行情数据模块
     * @return 初始化成功返回true，否则返回false
     */
    virtual bool initialize() = 0;

    /**
     * @brief 关闭行情数据模块
     */
    virtual void shutdown() = 0;

    /**
     * @brief 订阅订单簿数据
     * @param symbols 交易对列表，值为空时订阅全量行情
     * @param depth 订阅档位
     * @param cb 订单簿回调函数
     * @return 订阅成功返回true，否则返回false
     */
    virtual bool subscribe_orderbook(const Symbols& symbols, unsigned int depth, OrderbookCallback cb) = 0;

    /**
     * @brief 取消订阅订单簿
     */
    virtual void unsubscribe_orderbook() = 0;

    /**
     * @brief 获取交易对信息
     * @param cb 回调函数
     */
    virtual void fetch_pairs_info(ExPairInfoCallback cb) = 0;

    /**
     * @brief 获取资金费率
     * @param symbol 交易对符号
     * @param cb 回调函数
     */
    virtual void fetch_funding_fee(const Symbol& symbol, FundingFeeCallback cb) = 0;

    /**
     * @brief 获取订单簿快照
     * @param symbol 交易对符号
     * @return 订单簿智能指针
     */
    virtual SpOrderBook get_orderbook(const Symbol& symbol) const {
        Symbol pair = to_lower_str(symbol);
        auto it = orderbooks_.find(pair);
        return it != orderbooks_.end() ? it->second : nullptr;
    }

protected:
    /**
     * @brief 分发订单簿数据
     * @param ob 订单簿对象
     */
    void dispatch_orderbook(SpOrderBook ob) {
        if (orderbook_handler_)
            orderbook_handler_(ob);
    }
    /**
     * @brief 应用订单簿增量更新
     * @param is_full 是否全量更新
     * @param pair 交易对
     * @param milli 时间戳（毫秒）
     * @param asks 卖盘数据
     * @param bids 买盘数据
     * @param checksum 校验和
     * @param depth 深度
     * @return 更新后的订单簿
     */

    SpOrderBook apply_orderbook_delta(const std::string& pair, Timestamp milli, const double& ask_price,
                                      const double& ask_vol, const double& bid_price, const double& bid_vol) {
        auto it = orderbooks_.find(pair);
        if (it == orderbooks_.end()) [[unlikely]] {
            it = orderbooks_
                     .emplace(pair, std::make_shared<OrderBook>(pair, milli, ask_price, ask_vol, bid_price, bid_vol))
                     .first;
            return it->second;
        }
        auto& ob = it->second;
        if (milli < ob->update_milli) [[unlikely]]
            return ob;
        ob->update_milli = milli;
        ob->ask_price = ask_price;
        ob->ask_qty = ask_vol;
        ob->bid_price = bid_price;
        ob->bid_qty = bid_vol;
        return ob;
    }

protected:
    net::io_context& ioc_;         ///< IO上下文
    ssl::context& ssl_ctx_;        ///< SSL上下文
    AccountSecret account_secret_; ///< 账户密钥
    APIConfig base_config_;        ///< 配置信息

    OrderbookCallback orderbook_handler_;                ///< 订单簿回调函数
    std::unordered_map<Symbol, SpOrderBook> orderbooks_; ///< 订单簿缓存
};

/// 行情数据接口智能指针类型
using UpMarketData = std::unique_ptr<IExchangeMarketData>;

/**
 * @class IExchangeExecution
 * @brief 交易所交易执行接口
 * @details 定义交易执行相关操作的抽象接口，包括下单、撤单、订单查询等
 */
class IExchangeExecution {
public:
    /**
     * @brief 构造函数
     * @param ioc IO上下文
     * @param ssl_ctx SSL上下文
     * @param sec 账户密钥
     * @param config API配置
     */
    IExchangeExecution(net::io_context& ioc, ssl::context& ssl_ctx, const AccountSecret& sec, APIConfig config)
        : ioc_(ioc), ssl_ctx_(ssl_ctx), account_secret_(sec), base_config_(config) {}
    virtual ~IExchangeExecution() = default;

    /**
     * @brief 初始化交易执行模块
     * @return 初始化成功返回true，否则返回false
     */
    virtual bool initialize() = 0;

    /**
     * @brief 关闭交易执行模块
     */
    virtual void shutdown() = 0;

    /**
     * @brief 订阅订单更新
     * @param cb 订单回调函数
     * @return 订阅成功返回true，否则返回false
     */
    virtual bool subscribe_order(OrderCallback cb) = 0;

    /**
     * @brief 取消订阅订单更新
     */
    virtual void unsubscribe_order() = 0;

    /**
     * @brief 通过WebSocket接口下单
     * @param order 订单对象
     * @param cb 回调函数
     */
    virtual void place_order(const SpOrder& order, OrderCallback cb) = 0;

    /**
     * @brief 通过WebSocket接口撤单
     * @param order 订单对象
     * @param cb 回调函数
     */
    virtual void cancel_order(const SpOrder& order, OrderCallback cb) = 0;

    /**
     * @brief 查询订单状态
     * @param order 订单对象
     * @param cb 回调函数
     */
    virtual void query_order(const SpOrder& order, OrderCallback cb) = 0;

protected:
    /**
     * @brief 分发订单更新
     * @param order 订单对象
     */
    void dispatch_order(SpOrder order) const {
        if (order_handler_)
            order_handler_(Errno::Ok, order);
    }

protected:
    net::io_context& ioc_;                                   ///< IO上下文
    ssl::context& ssl_ctx_;                                  ///< SSL上下文
    AccountSecret account_secret_;                           ///< 账户密钥
    APIConfig base_config_;                                  ///< 配置信息
    OrderCallback order_handler_;                            ///< 订单回调函数
};

/// 交易执行接口智能指针类型
using UpExecution = std::unique_ptr<IExchangeExecution>;

/**
 * @class IExchangeAccount
 * @brief 交易所账户接口
 * @details 定义账户相关操作的抽象接口，包括余额查询、持仓查询、杠杆设置等
 */
class IExchangeAccount {
public:
    /**
     * @brief 构造函数
     * @param ioc IO上下文
     * @param ssl_ctx SSL上下文
     * @param sec 账户密钥
     * @param config API配置
     */
    IExchangeAccount(net::io_context& ioc, ssl::context& ssl_ctx, const AccountSecret& sec, APIConfig config)
        : ioc_(ioc), ssl_ctx_(ssl_ctx), account_secret_(sec), base_config_(config) {};
    virtual ~IExchangeAccount() = default;

    /**
     * @brief 初始化账户模块
     * @return 初始化成功返回true，否则返回false
     */
    virtual bool initialize() = 0;

    /**
     * @brief 关闭账户模块
     */
    virtual void shutdown() = 0;

    /**
     * @brief 异步获取货币余额
     * @param currency 货币符号
     * @param cb 回调函数
     */
    virtual void get_balance(const Currency& currency, BalanceCallback cb) = 0;

    /**
     * @brief 异步获取交易对持仓
     * @param symbol 交易对符号
     * @param cb 回调函数
     */
    virtual void get_position(const Symbol& symbol, PositionCallback cb) = 0;

    /**
     * @brief 异步获取MR
     * @param symbol 交易对符号
     * @param cb 回调函数
     */
    virtual void get_margin_ratio(MarginRatioCallback cb) = 0;

    /**
     * @brief 设置杠杆倍数
     * @param symbol 交易对符号
     * @param leverage 杠杆倍数
     * @param mode 保证金模式
     * @return 设置成功返回true，否则返回false
     */
    virtual void set_leverage(const Symbol& symbol, unsigned int leverage, MarginMode mode, LeverageCallback cb) = 0;

protected:
    net::io_context& ioc_;         ///< IO上下文
    ssl::context& ssl_ctx_;        ///< SSL上下文
    AccountSecret account_secret_; ///< 账户密钥
    APIConfig base_config_;        ///< 配置信息
};

/// 账户接口智能指针类型
using UpAccount = std::unique_ptr<IExchangeAccount>;

} // namespace infra
