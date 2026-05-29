/**
 * @file structs.h
 * @brief 数据结构定义
 * @details 定义交易所相关的所有数据结构，包括资产、持仓、订单等
 */

#pragma once

#include <memory>
#include <string>
#include <vector>
#include <fmt/format.h>
#include "utils.h"

namespace infra {

/**
 * @struct Balance
 * @brief 资产信息结构
 */
struct Balance {
    Currency currency;        ///< 货币符号
    double available{0.0};    ///< 可用余额
    double freeze{0.0};       ///< 冻结余额
    double borrow{0.0};       ///< 借款金额
    double interest{0.0};     ///< 借款利息
    double withdraw{0.0};     ///< 可提现金额
    double realised_pnl{0.0}; ///< 已实现盈亏

    /**
     * @brief 计算总权益
     * @return 总权益
     */
    double equity() const { return available + freeze + borrow + interest; }

    /**
     * @brief 构造函数
     * @param c 货币符号
     * @param a 可用余额
     * @param f 冻结余额
     */
    Balance(std::string c, double a, double f) : currency(std::move(c)), available(a), freeze(f) {}

    std::string to_json() const {
        return fmt::format(R"({{"currency":"{}","available":"{}","freeze":"{}","withdraw":"{}","realised_pnl":"{}"}})",
                           currency, available, freeze, withdraw, realised_pnl);
    }
};

/// 资产信息智能指针类型
using SpBalance = std::shared_ptr<Balance>;

/**
 * @struct Position
 * @brief 持仓信息结构
 */
struct Position {
    PositionMode position_mode{PositionMode::one_way_mode}; ///< 持仓模式
    MarginMode margin_mode{MarginMode::CROSS};              ///< 保证金模式
    Symbol symbol;                                          ///< 交易对
    Timestamp update_time{0};                               ///< 更新时间
    double long_size{0.0};                                  ///< 多仓数量
    double long_open_price{0.0};                            ///< 多仓开仓价
    double short_size{0.0};                                 ///< 空仓数量
    double short_open_price{0.0};                           ///< 空仓开仓价
    double bankrupt_price{0.0};                             ///< 强平价
    double open_exp_profit{0.0};                            ///< 开仓期望盈利
    unsigned int leverage{1};                               ///< 杠杆倍数

    Position() {}
    Position(const Symbol& pair, const double& Long, const double& long_open_price, const double& Short,
             const double& short_open_price, const Timestamp milli, const double& bankrupt) {
        this->update_time = milli;
        this->symbol = pair;
        this->long_size = Long;
        this->long_open_price = long_open_price;
        this->short_size = Short;
        this->short_open_price = short_open_price;
        this->bankrupt_price = bankrupt;
    }

    double net() const { return this->long_size - this->short_size; }

    std::string to_json() const {
        return fmt::format(
            R"({{"symbol":"{}","update_time":{},"long_size":"{}@{}","short_size":"{}@{}","bankrupt_price":{},"open_exp_profit":{}}})",
            symbol, time_milli_to_iso(update_time), long_size, long_open_price, short_size, short_open_price,
            bankrupt_price, open_exp_profit);
    }
};

/// 持仓信息智能指针类型
using SpPosition = std::shared_ptr<Position>;

/**
 * @struct AccountSecret
 * @brief 账户密钥结构
 */
struct AccountSecret {
    std::string secret_id;
    std::string client;
    std::string username;
    Exchange exchange;
    std::string field = "swap";
    Settlement settlement;

    std::string api_key;        ///< API密钥
    std::string api_secret;     ///< API秘钥
    std::string api_phrase;     ///< API口令
    std::string wallet_address; ///< 钱包地址

    std::unordered_map<std::string, std::string> custom_info; // 某些所特有的字段

    std::string to_json() const {
        return fmt::format(R"({{"client":"{}","username":{},"exchange":{}}})", client, username,
                           exchange_to_text(exchange));
    }
};

struct ConnectData {
    std::string host; ///< 主机地址
    std::string port; ///< 端口号
    std::string path; ///< 路径
};

/**
 * @struct APIConfig
 * @brief API配置信息结构
 */
struct APIConfig {
    Exchange name;                                   ///< 交易所名称
    AccountType account_type;                        ///< 交易类型，如现货，永续合约，交割合约等
    AddressType address_type;                        ///< 地址类型
    Settlement settle_unit;                          ///< 结算货币
    AccountMode account_mode = AccountMode::CLASSIC; ///< 账户模式，如经典账户，统一账户

    APIConfig() = default;
    APIConfig(Exchange name, AccountType account_type, AddressType address_type, Settlement settle_unit,
              AccountMode account_mode = AccountMode::CLASSIC)
        : name(name), account_type(account_type), address_type(address_type), settle_unit(settle_unit),
          account_mode(account_mode) {}

    std::string to_str() const {
        return (exchange_to_text(name) + to_string(account_type) + to_string(address_type) + to_string(settle_unit));
    }
};

// 签名结果结构体
struct EcdsaSignature {
    std::string r_hex;
    std::string s_hex;
    int v;
    std::string hex;
};

/**
 * @struct ExchangePairInfo
 * @brief 交易对信息结构
 */
struct ExchangePairInfo {
    Symbol pair;                  ///< 交易对，infra格式
    double trading_min_base;      ///< 最小下单数量，单位是币数
    double step_size_base;        ///< 最小头寸变动单位
    double step_size_quote;       ///< 最小价格波动单位
    double min_size_quote{1};     ///< 最小名义价值
    double denomination_value{1}; ///< 合约面值
    std::string alias;            ///< 交易对，exchange格式

    std::string to_json() const {
        return fmt::format(
            R"({{"symbol":"{}","trading_min_base":{},"step_size_base":{},"step_size_quote":{},"min_size_quote":{},"denomination_value":{},"alias":"{}"}})",
            pair, trading_min_base, step_size_base, step_size_quote, min_size_quote, denomination_value, alias);
    }
};

/// 交易对信息智能指针类型
using SpExPairInfo = std::shared_ptr<ExchangePairInfo>;

/**
 * @struct FundingFee
 * @brief 资金费率结构
 */
struct FundingFee {
    Symbol pair;             ///< 交易对
    Timestamp milli;         ///< 时间戳
    double fee;              ///< 资金费率
    Timestamp next_milli{0}; ///< 下次资金费时间
    double next_fee{0};      ///< 下次资金费率

    FundingFee(std::string pair) : pair(std::move(pair)) { milli = time_get_now_milli(); }

    /**
     * @brief 构造函数
     * @param pair 交易对
     * @param m 时间戳
     * @param f 资金费率
     */
    FundingFee(std::string pair, int64_t m, double f) : pair(std::move(pair)), milli(m), fee(f) {}

    /**
     * @brief 构造函数（包含下次资金费）
     * @param pair 交易对
     * @param m 时间戳
     * @param f 资金费率
     * @param next_m 下次资金费时间
     * @param next_f 下次资金费率
     */
    FundingFee(std::string pair, int64_t m, double f, int64_t next_m, double next_f)
        : pair(std::move(pair)), milli(m), fee(f), next_milli(next_m), next_fee(next_f) {}
};

/// 资金费率智能指针类型
using SpFundingFee = std::shared_ptr<FundingFee>;

/**
 * @struct OrderFill
 * @brief 订单成交明细结构
 */
struct OrderFill {
    Timestamp milli; ///< 成交时间
    std::string id;  ///< 成交ID
    double quantity; ///< 成交数量
    double price;    ///< 成交价格
};

/**
 * @struct ObLatency
 * @brief 行情延迟追踪结构
 */
struct ObLatency {
    Timestamp ex_milli = 0;   ///< 交易所行情时间戳(ms)
    Timestamp recv_milli = 0; ///< 本地接收行情时间戳(ms)
    uint64_t recv_tsc = 0;    ///< 行情系统接收时间
    uint64_t parsed_tsc = 0;  ///< 行情解析完成时间

    void update(Timestamp ex, Timestamp recv_ms, uint64_t recv, uint64_t parsed) noexcept {
        ex_milli = ex;
        recv_milli = recv_ms;
        recv_tsc = recv;
        parsed_tsc = parsed;
    }
};

/**
 * @struct OrderTsc
 * @brief 单笔订单时间戳追踪
 */

struct OrderTsc {
    uint64_t send_tsc   = 0; ///< 发单时间
    uint64_t sent_tsc   = 0; ///< 发单后时间
    uint64_t ack_tsc    = 0; ///< 交易所首次回报时间
    uint64_t fill_tsc   = 0; ///< 成交时间
};

/**
 * @struct TradeLatency
 * @brief 全链路延迟追踪结构
 */
struct TradeLatency {
    ClientOrderId cid;       ///< master订单唯一标识
    ObLatency master_ob;     ///< master行情延迟
    ObLatency slave_ob;      ///< slave行情延迟
    OrderTsc master_order;   ///< master订单时间戳
    OrderTsc slave_order;    ///< slave订单时间戳
};
using SpTradeLatency = std::shared_ptr<TradeLatency>;

/**
 * @struct Order
 * @brief 订单信息结构
 */
struct Order {
    Timestamp milli = time_get_now_milli();          ///< 更新时间戳
    Timestamp creation_milli = time_get_now_milli(); ///< 创建时间戳

    Timestamp exchange_create_time = 0; ///< 交易所创建时间戳
    Timestamp exchange_update_time = 0; ///< 交易所更新时间戳

    Symbol pair;              ///< 交易对
    ClientOrderId client_oid; ///< 系统订单ID
    OrderId market_oid;       ///< 交易所订单ID
    unsigned long uid{0};     ///< 递增序列值

    OrderSide side = OrderSide::OpenLong;      ///< 订单方向
    OrderType type = OrderType::Limit;         ///< 订单类型
    OrderTIF tif = OrderTIF::GTC;              ///< 时间条件
    OrderStatus status = OrderStatus::Created; ///< 订单状态

    double price{0};     ///< 订单价格
    double quantity{0};  ///< 订单数量
    double avg_price{0}; ///< 平均成交价

    double cum_deal_base{0};    ///< 累计成交数量
    double delta_deal_base{0};  ///< 增量成交数量
    double cum_deal_quote{0};   ///< 累计成交金额
    double delta_deal_quote{0}; ///< 增量成交金额

    double net_base{0};  ///< 净数量
    double net_quote{0}; ///< 净金额

    bool reduce_only = false;       ///< 是否仅减仓
    std::string par_leverage = "1"; ///< 杠杆倍数

    double stop_loss_trigger_price{0};   ///< 止损触发价
    double take_profit_trigger_price{0}; ///< 止盈触发价

    Errno ec = Errno::Ok; ///< 错误码
    std::string detail;   ///< 错误详情

    std::vector<OrderFill> fills; ///< 成交明细列表
    SpTradeLatency latency;       ///< 全链路延迟追踪

    /**
     * @brief 默认构造函数
     */
    Order() { latency = std::make_shared<TradeLatency>(); }

    /**
     * @brief 构造函数
     * @param p 交易对
     * @param cid 客户订单ID
     * @param mid 交易所订单ID
     */
    Order(Symbol p, ClientOrderId cid, OrderId mid)
        : pair(std::move(p)), client_oid(std::move(cid)), market_oid(std::move(mid)) {
        latency = std::make_shared<TradeLatency>();
    }

    /**
     * @brief 构造函数
     * @param p 交易对
     * @param cid 客户订单ID
     * @param mid 交易所订单ID
     * @param s 订单方向
     * @param t 订单类型
     * @param tf 时间条件
     * @param pr 价格
     * @param qty 数量
     */
    Order(Symbol p, ClientOrderId cid, OrderId mid, OrderSide s, OrderType t, OrderTIF tf, double pr, double qty)
        : pair(std::move(p)), client_oid(std::move(cid)), market_oid(std::move(mid)), side(s), type(t), tif(tf),
          price(pr), quantity(qty) {
        latency = std::make_shared<TradeLatency>();
    }

    /**
     * @brief 更新订单信息
     * @param other 新的订单信息
     * @return 是否发生状态转换
     */
    bool update(const Order& other) {
        delta_deal_base = 0;
        delta_deal_quote = 0;

        bool status_transit =
            (other.status != this->status || other.cum_deal_base > this->cum_deal_base || other.milli >= this->milli);

        if (this->status == OrderStatus::Failed || this->status == OrderStatus::Filled ||
            (this->status == OrderStatus::Canceled && other.status == OrderStatus::Amending) ||
            (this->status == OrderStatus::Amending &&
             static_cast<int>(other.status) <= static_cast<int>(OrderStatus::New)))
            status_transit = false;

        ec = other.ec;
        if (ec != Errno::Ok && !other.detail.empty())
            detail = other.detail;
        if (!status_transit)
            return false;

        // === 成交明细链 ===
        if (other.cum_deal_base > cum_deal_base) {
            double base_delta = other.cum_deal_base - cum_deal_base;
            double quote_delta = other.cum_deal_quote - cum_deal_quote;
            fills.push_back({other.milli, "", base_delta, other.avg_price});
            delta_deal_base = base_delta;
            delta_deal_quote = quote_delta;
        }

        // === 同步字段 ===
        if (other.milli)
            milli = other.milli;
        if (!other.market_oid.empty())
            market_oid = other.market_oid;
        status = other.status;
        if (other.quantity)
            quantity = other.quantity;
        if (other.price)
            price = other.price;
        cum_deal_base = std::max(cum_deal_base, other.cum_deal_base);
        cum_deal_quote = std::max(cum_deal_quote, other.cum_deal_quote);
        if (other.avg_price)
            avg_price = other.avg_price;
        if (other.net_base)
            net_base = other.net_base;
        if (other.net_quote)
            net_quote = other.net_quote;
        if (other.exchange_create_time)
            exchange_create_time = other.exchange_create_time;
        if (other.exchange_update_time)
            exchange_update_time = other.exchange_update_time;

        return true;
    }

    /**
     * @brief 序列化为JSON字符串
     * @return JSON字符串
     */
    std::string to_json() const {
        std::string fills_str;
        for (size_t i = 0; i < fills.size(); ++i) {
            const auto& f = fills[i];
            fmt::format_to(std::back_inserter(fills_str), R"({{"milli":{},"id":"{}","quantity":{},"price":{}}}{})",
                           f.milli, f.id, f.quantity, f.price, i + 1 < fills.size() ? "," : "");
        }
        return fmt::format(
            R"({{"pair":"{}","client_oid":"{}","market_oid":"{}","uid":"{}","side":"{}","type":"{}","tif":"{}","status":"{}","price":{},"quantity":{},"avg_price":{},"cum_deal_base":{},"cum_deal_quote":{},"delta_deal_base":{},"delta_deal_quote":{},"net_base":{},"net_quote":{},"reduce_only":{},"par_leverage":"{}","stop_loss_trigger_price":{},"take_profit_trigger_price":{},"ec":"{}","detail":"{}","creation_milli":{},"milli":{},"exchange_create_time":{},"exchange_update_time":{},"fills":[{}]}})",
            pair, client_oid, market_oid, uid, to_string(side), to_string(type), to_string(tif), to_string(status),
            price, quantity, avg_price, cum_deal_base, cum_deal_quote, delta_deal_base, delta_deal_quote, net_base,
            net_quote, reduce_only ? "true" : "false", par_leverage, stop_loss_trigger_price, take_profit_trigger_price,
            to_string(ec), detail, creation_milli, milli, exchange_create_time, exchange_update_time, fills_str);
    }
};

/// 订单智能指针类型
using SpOrder = std::shared_ptr<Order>;

} // namespace infra