/**
 * @file types.h
 * @brief 类型定义
 * @details 定义交易所相关的所有枚举类型和别名
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace infra {

/// 交易对符号类型
using Symbol = std::string;
/// 交易对列表类型
using Symbols = std::vector<Symbol>;
/// 货币符号类型
using Currency = std::string;
/// 交易所订单ID类型
using OrderId = std::string;
/// 客户订单ID类型
using ClientOrderId = std::string;
/// 时间戳类型（毫秒）
using Timestamp = int64_t;

/**
 * @enum Exchange
 * @brief 交易所枚举
 */
enum class Exchange : uint8_t {
    UNKNOWN = 0,
    APEX,
    ASTER,
    BACKPACK,
    BINANCE,
    BINGX,
    BITGET,
    BITMART,
    BITUNIX,
    BYBIT,
    COINEX,
    CROSSEX_GATE,
    DEXLESS,
    EDGEX,
    GATE,
    HBG,
    HYPERLIQUID,
    KUCOIN,
    LIGHTER,
    LTP_BINANCE,
    NADO,
    OKEX,
    ORANGEX,
    PARADEX,
    PHEMEX,
    TOOBIT,
    WEEX,
    XT
};

/**
 * @enum AccountMode
 * @brief 账户模式枚举
 */
enum class AccountMode : uint8_t { CLASSIC = 0, UNIFIED };

/**
 * @enum AccountType
 * @brief 账户类型枚举
 */
enum class AccountType : uint8_t { SPOT = 0, SWAP, MARGIN, FUTURES };

/**
 * @enum AddressType
 * @brief 地址类型枚举
 */
enum class AddressType : uint8_t { NORMAL = 0, COLO, VIP, SPECIAL };

/**
 * @enum Settlement
 * @brief 结算货币枚举
 */
enum class Settlement : uint8_t { USDT = 0, USDC, USD };

/**
 * @enum MarginMode
 * @brief 保证金模式枚举（逐仓/全仓）
 */
enum class MarginMode : uint8_t { ISOLATED = 0, CROSS };

/**
 * @enum PositionMode
 * @brief 持仓模式枚举（单向/双向）
 */
enum class PositionMode : uint8_t { hedge_mode = 0, one_way_mode };

/**
 * @enum OrderSide
 * @brief 订单方向枚举
 */
enum class OrderSide {
    OpenLong = 0x01,
    OpenShort = 0x01 << 1,
    CloseShort = 0x01 << 2,
    CloseLong = 0x01 << 3,
    ALL = (0x01) | (0x01 << 1) | (0x01 << 2) | (0x01 << 3),
    CloseOnly = (0x01 << 2) | (0x01 << 3),
    OpenOnly = (0x01) | (0x01 << 1),
    DntOpenLong = (0x01 << 1) | (0x01 << 2) | (0x01 << 3),
    DntOpenShort = (0x01) | (0x01 << 2) | (0x01 << 3),
    NONE = 0
};

/**
 * @enum OrderStatus
 * @brief 订单状态枚举
 */
enum class OrderStatus : uint8_t {
    Created = 0,     // 订单创建
    New,             // 下单请求被接收
    PartiallyFilled, // 部成
    Filled,          // 全成
    Canceling,       // 撤单请求被接收
    Canceled,        // 已撤
    Amending,        // amend请求被接收
    Rejected,
    Expired,
    Failed,
};

/**
 * @enum OrderTIF
 * @brief 订单时间条件枚举
 */
enum class OrderTIF : uint8_t {
    GTC = 0, // goods till cancel
    MAKER,   // post only
    IOC,     // immediately or cancel
    FOK,     // full or kill
};

/**
 * @enum OrderType
 * @brief 订单类型枚举
 */
enum class OrderType : uint8_t {
    Limit = 0,
    Market,
    StopLoss,
    TakeProfit,
};

/**
 * @enum Errno
 * @brief 错误码枚举
 */
enum class Errno : int32_t {
    Ok = 0,
    UnknownError = 110000,

    // 参数和请求错误 (100100-100199)
    InvalidParams = 100100,
    RequestTimeout = 100101,
    TimestampAhead = 100102,

    // 认证和安全错误 (100300-100399)
    AuthFailed = 100300,
    NotInWhiteList = 100301,

    // 订单和交易错误 (100400-100499)
    OrderNotFound = 100400,
    DuplicatedId = 100401,
    PositionSideWrong = 100402,
    InsufficientBalance = 100403,
    SmallSizeOrder = 100404,
    ReduceOnlyRejected = 100405,
    PostOnlyRejected = 100406,
    RateLimitExceed = 100407,
    PositionFull = 100408,
    InsufficientBorrow = 100409,
    PercentPrice = 100410,
    IocRejected = 100411,
    FokRejected = 100412,

    // 系统错误 (100500-100599)
    NotImplemented = 100500,
    NotFoundDenomination = 100501,
    CriticalError = 100502,
    NullPtr = 100503,
    InternalError = 100504,
    NotInit = 100505,
    NotSupported = 100506,
    ParseJsonFailed = 100507,
    ExchangeBusyNow = 100508,
    NetworkFailed = 100509,
    NetworkError = 100510,
    ApiError = 100511,
};

} // namespace infra