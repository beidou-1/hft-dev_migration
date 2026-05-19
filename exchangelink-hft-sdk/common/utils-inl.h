/**
 * @file utils-inl.h
 * @brief 工具函数集
 */

#pragma once

#include <cmath>
#include <ctime>
#include <chrono>
#include <random>
#include <algorithm>
#include <iostream>
#include <sstream>
#include <iomanip>
#include <fcntl.h>
#include <unistd.h>
#include <boost/date_time/posix_time/posix_time.hpp>

namespace infra {

inline Timestamp time_get_now_sec() noexcept {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return ts.tv_sec;
}

inline Timestamp time_get_now_milli() noexcept {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return ts.tv_sec * 1'000 + ts.tv_nsec / 1'000'000;
}

inline Timestamp time_get_now_micro() noexcept {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return ts.tv_sec * 1'000'000 + ts.tv_nsec / 1'000;
}

inline Timestamp time_get_now_nano() noexcept {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return ts.tv_sec * 1'000'000'000 + ts.tv_nsec;
}

inline std::string time_get_now_str() {
    auto now = std::chrono::system_clock::now();
    std::time_t tt = std::chrono::system_clock::to_time_t(now);
    std::tm utc_tm = {};
    gmtime_r(&tt, &utc_tm);
    std::ostringstream ss;
    ss << std::put_time(&utc_tm, "%Y-%m-%dT%H:%M:%S");
    return ss.str();
}

inline std::string time_milli_to_iso(Timestamp milli) {
    const int milli_part = milli % 1000;
    boost::posix_time::ptime ptime = boost::posix_time::from_time_t(milli / 1000);
    return boost::posix_time::to_iso_extended_string(ptime) + "." + std::to_string(milli_part) + "Z";
}

inline Timestamp time_iso_to_milli(const char* text) {
    int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0, milli = 0;
    sscanf(text, "%04d-%02d-%02dT%02d:%02d:%02d.%dZ", &year, &month, &day, &hour, &minute, &second, &milli);
    struct tm time = {second, minute, hour, day, month - 1, year - 1900};
    int milli_width = 3;
    const char* Z_pos = strstr(text, "Z");
    char* milli_start_pos = (char*)Z_pos - 1;
    while (true) {
        if ((*milli_start_pos) == '.') {
            break;
        } else if ((*milli_start_pos) == ':') {
            milli = 0;
            milli_start_pos = nullptr;
            break;
        }
        --milli_start_pos;
    }
    if (milli_start_pos != nullptr) {
        milli_width = (int)(Z_pos - milli_start_pos - 1);
        switch (milli_width) {
            case 1:
                milli = milli * 100;
                break;
            case 2:
                milli = milli * 10;
                break;
            case 6:
                milli = milli / 1000;
                break;
        }
    }
    Timestamp time_milli = ((Timestamp)timegm(&time)) * 1000 + milli;
    return time_milli;
}

namespace tsc_detail {
inline double& tsc_ns_per_cycle() {
    static double val = 1.0;
    return val;
}
} // namespace tsc_detail

inline void tsc_calibrate() noexcept {
    struct timespec t1, t2;
    uint64_t c1, c2;
    clock_gettime(CLOCK_MONOTONIC_RAW, &t1);
    c1 = rdtsc();
    // 睡眠 10ms 作为标定窗口
    struct timespec sleep_ts = {0, 10000000};
    nanosleep(&sleep_ts, nullptr);
    c2 = rdtscp();
    clock_gettime(CLOCK_MONOTONIC_RAW, &t2);
    uint64_t ns_elapsed = (t2.tv_sec - t1.tv_sec) * 1000000000ULL + (t2.tv_nsec - t1.tv_nsec);
    uint64_t cycles_elapsed = c2 - c1;
    tsc_detail::tsc_ns_per_cycle() = (double)ns_elapsed / (double)cycles_elapsed;
}

inline uint64_t tsc_to_ns(uint64_t cycles) noexcept { return (uint64_t)(cycles * tsc_detail::tsc_ns_per_cycle()); }

inline double get_cpu_freq_ghz() noexcept { return 1.0 / tsc_detail::tsc_ns_per_cycle(); }

inline std::string to_string(Errno err) {
    switch (err) {
        case Errno::Ok:
            return "OK";
        case Errno::InvalidParams:
            return "INVALID_PARAMS";
        case Errno::RequestTimeout:
            return "REQUEST_TIMEOUT";
        case Errno::TimestampAhead:
            return "TIMESTAMP_AHEAD";
        case Errno::AuthFailed:
            return "AUTH_FAILED";
        case Errno::NotInWhiteList:
            return "NOT_IN_WHITELIST";
        case Errno::OrderNotFound:
            return "ORDER_NOT_FOUND";
        case Errno::DuplicatedId:
            return "DUPLICATED_ID";
        case Errno::PositionSideWrong:
            return "POSITION_SIDE_WRONG";
        case Errno::InsufficientBalance:
            return "NO_BALANCE";
        case Errno::SmallSizeOrder:
            return "TOO_SMALL_SIZE";
        case Errno::ReduceOnlyRejected:
            return "REDUCE_ONLY_REJECTED";
        case Errno::PostOnlyRejected:
            return "POST_ONLY_REJECTED";
        case Errno::RateLimitExceed:
            return "RATE_LIMIT_EXCEED";
        case Errno::PositionFull:
            return "POSITION_FULL";
        case Errno::InsufficientBorrow:
            return "INSUFFICIENT_BORROW";
        case Errno::PercentPrice:
            return "PERCENT_PRICE";
        case Errno::IocRejected:
            return "IOC_REJECTED";
        case Errno::FokRejected:
            return "FOK_REJECTED";
        case Errno::NotImplemented:
            return "NOT_IMPLEMENTED";
        case Errno::NotFoundDenomination:
            return "NOT_FOUND_DENOMINATION_VALUE";
        case Errno::CriticalError:
            return "CRITICAL_ERROR";
        case Errno::NullPtr:
            return "NULL_POINTER";
        case Errno::InternalError:
            return "INTERNAL_ERROR";
        case Errno::NotInit:
            return "NOT_INIT";
        case Errno::NotSupported:
            return "NOT_SUPPORTED";
        case Errno::ParseJsonFailed:
            return "PARSE_JSON_FAILED";
        case Errno::ExchangeBusyNow:
            return "EXCHANGE_BUSY";
        case Errno::NetworkFailed:
            return "NETWORK_FAILED";
        case Errno::NetworkError:
            return "NETWORK_ERROR";
        case Errno::ApiError:
            return "API_ERROR";
        default:
            return "UNKNOWN_ERROR";
    }
}

inline std::string to_string(AccountMode m) {
    switch (m) {
        case AccountMode::CLASSIC:
            return "CLASSIC";
        case AccountMode::UNIFIED:
            return "UNIFIED";
        default:
            return "UNKNOWN";
    }
}

inline std::string to_string(AccountType t) {
    switch (t) {
        case AccountType::SPOT:
            return "SPOT";
        case AccountType::SWAP:
            return "SWAP";
        case AccountType::MARGIN:
            return "MARGIN";
        case AccountType::FUTURES:
            return "FUTURES";
        default:
            return "UNKNOWN";
    }
}

inline std::string to_string(AddressType t) {
    switch (t) {
        case AddressType::NORMAL:
            return "NORMAL";
        case AddressType::COLO:
            return "COLO";
        case AddressType::VIP:
            return "VIP";
        case AddressType::SPECIAL:
            return "SPECIAL";
        default:
            return "UNKNOWN";
    }
}

inline std::string to_string(Settlement t) {
    switch (t) {
        case Settlement::USDT:
            return "USDT";
        case Settlement::USD:
            return "USD";
        case Settlement::USDC:
            return "USDC";
        default:
            return "UNKNOWN";
    }
}

inline std::string to_string(MarginMode m) {
    switch (m) {
        case MarginMode::ISOLATED:
            return "ISOLATED";
        case MarginMode::CROSS:
            return "CROSSED";
        default:
            return "UNKNOWN";
    }
}

inline std::string to_string(PositionMode m) {
    switch (m) {
        case PositionMode::hedge_mode:
            return "HEDGE_MODE";
        case PositionMode::one_way_mode:
            return "ONE_WAY_MODE";
        default:
            return "UNKNOWN";
    }
}

inline std::string to_string(OrderSide s) {
    switch (s) {
        case OrderSide::OpenLong:
            return "OPEN_LONG";
        case OrderSide::OpenShort:
            return "OPEN_SHORT";
        case OrderSide::CloseShort:
            return "CLOSE_SHORT";
        case OrderSide::CloseLong:
            return "CLOSE_LONG";
        default:
            return "UNKNOWN";
    }
}

inline std::string to_string(OrderStatus s) {
    switch (s) {
        case OrderStatus::Created:
            return "CREATED";
        case OrderStatus::New:
            return "NEW";
        case OrderStatus::PartiallyFilled:
            return "PARTIALLY_FILLED";
        case OrderStatus::Filled:
            return "FILLED";
        case OrderStatus::Canceling:
            return "CANCELING";
        case OrderStatus::Canceled:
            return "CANCELED";
        case OrderStatus::Amending:
            return "AMENDING";
        case OrderStatus::Rejected:
            return "REJECTED";
        case OrderStatus::Expired:
            return "EXPIRED";
        case OrderStatus::Failed:
            return "FAILED";
        default:
            return "UNKNOWN";
    }
}

inline std::string to_string(OrderTIF t) {
    switch (t) {
        case OrderTIF::GTC:
            return "GTC";
        case OrderTIF::MAKER:
            return "GTX";
        case OrderTIF::IOC:
            return "IOC";
        case OrderTIF::FOK:
            return "FOK";
        default:
            return "UNKNOWN";
    }
}

inline std::string to_string(OrderType t) {
    switch (t) {
        case OrderType::Limit:
            return "LIMIT";
        case OrderType::Market:
            return "MARKET";
        case OrderType::StopLoss:
            return "STOP_LOSS";
        case OrderType::TakeProfit:
            return "TAKE_PROFIT";
        default:
            return "UNKNOWN";
    }
}

inline MarginMode to_margin_mode(std::string_view sv) {
    if (sv == "isolated" || sv == "ISOLATED") {
        return MarginMode::ISOLATED;
    } else {
        return MarginMode::CROSS;
    }
}

inline OrderStatus to_order_status(std::string_view sv) {
    if (sv == "NEW" || sv == "OPEN" || sv == "PENDING" || sv == "LIVE") {
        return OrderStatus::New;
    } else if (sv == "PARTIALLY_FILLED") {
        return OrderStatus::PartiallyFilled;
    } else if (sv == "FILLED") {
        return OrderStatus::Filled;
    } else if (sv == "CANCELED" || sv == "CANCELLED") {
        return OrderStatus::Canceled;
    } else if (sv == "REJECTED" || sv == "REJECT") {
        return OrderStatus::Rejected;
    } else if (sv == "EXPIRED" || sv == "EXPIRED_IN_MATCH") {
        return OrderStatus::Expired;
    } else {
        return OrderStatus::Failed;
    }
}

inline OrderTIF to_order_tif(std::string_view sv) {
    if (sv == "GTC" || sv == "GTE_GTC") {
        return OrderTIF::GTC;
    } else if (sv == "IOC") {
        return OrderTIF::IOC;
    } else if (sv == "FOK") {
        return OrderTIF::FOK;
    } else if (sv == "GTX" || sv == "MAKER" || sv == "POST_ONLY") {
        return OrderTIF::MAKER;
    } else {
        return OrderTIF::GTC;
    }
}

inline OrderType to_order_type(std::string_view sv) {
    if (sv == "LIMIT") {
        return OrderType::Limit;
    } else if (sv == "MARKET") {
        return OrderType::Market;
    } else if (sv == "STOP" || sv == "STOP_MARKET") {
        return OrderType::StopLoss;
    } else if (sv == "TAKE_PROFIT" || sv == "TAKE_PROFIT_MARKET") {
        return OrderType::TakeProfit;
    } else {
        return OrderType::Limit;
    }
}

inline OrderSide to_order_side(std::string_view side, std::string_view position_side) {
    if (side == "BUY" && position_side == "LONG") {
        return OrderSide::OpenLong;
    } else if (side == "SELL" && position_side == "LONG") {
        return OrderSide::CloseLong;
    } else if (side == "SELL" && position_side == "SHORT") {
        return OrderSide::OpenShort;
    } else {
        return OrderSide::CloseShort;
    }
}

inline const char* order_side_to_text(OrderSide side) {
    switch (side) {
        case OrderSide::OpenLong:
            return "buy";
        case OrderSide::OpenShort:
            return "sell";
        case OrderSide::CloseShort:
            return "closeshort";
        case OrderSide::CloseLong:
            return "closelong";
        case OrderSide::NONE:
            return "none";
        default:
            return "unknown";
    }
}

inline OrderSide order_side_from_text(std::string_view side_text) {
    if (side_text == "buy" || side_text == "Buy" || side_text == "BUY") {
        return OrderSide::OpenLong;
    } else if (side_text == "sell" || side_text == "Sell" || side_text == "SELL") {
        return OrderSide::OpenShort;
    } else if (side_text == "closeshort") {
        return OrderSide::CloseShort;
    } else if (side_text == "closelong") {
        return OrderSide::CloseLong;
    } else {
        return OrderSide::NONE;
    }
}

inline const char* allowence_to_text(OrderSide side) {
    switch (side) {
        case OrderSide::OpenLong:
            return "OpenLong";
        case OrderSide::OpenShort:
            return "OpenShort";
        case OrderSide::OpenOnly:
            return "OpenOnly";
        case OrderSide::CloseOnly:
            return "CloseOnly";
        case OrderSide::ALL:
            return "ALL";
        case OrderSide::NONE:
            return "NONE";
        default:
            return "unknown";
    }
}

inline OrderSide allowence_from_text(std::string_view side_text) {
    if (side_text == "OpenLong" || side_text == "openlong") {
        return OrderSide::OpenLong;
    } else if (side_text == "OpenShort" || side_text == "openshort") {
        return OrderSide::OpenShort;
    } else if (side_text == "OpenOnly" || side_text == "openonly") {
        return OrderSide::OpenOnly;
    } else if (side_text == "CloseOnly" || side_text == "closeonly") {
        return OrderSide::CloseOnly;
    } else if (side_text == "ALL" || side_text == "all") {
        return OrderSide::ALL;
    } else if (side_text == "None" || side_text == "none") {
        return OrderSide::NONE;
    } else {
        return OrderSide::NONE;
    }
}

inline const char* exchange_to_text(Exchange exchange) {
    switch (exchange) {
        case Exchange::APEX:
            return "apex";
        case Exchange::ASTER:
            return "aster";
        case Exchange::BACKPACK:
            return "backpack";
        case Exchange::BINANCE:
            return "binance";
        case Exchange::BINGX:
            return "bingx";
        case Exchange::BITGET:
            return "bitget";
        case Exchange::BITMART:
            return "bitmart";
        case Exchange::BITUNIX:
            return "bitunix";
        case Exchange::BYBIT:
            return "bybit";
        case Exchange::COINEX:
            return "coinex";
        case Exchange::CROSSEX_GATE:
            return "crossexgate";
        case Exchange::DEXLESS:
            return "dexless";
        case Exchange::EDGEX:
            return "edgex";
        case Exchange::GATE:
            return "gate";
        case Exchange::HBG:
            return "hbg";
        case Exchange::HYPERLIQUID:
            return "hyperliquid";
        case Exchange::KUCOIN:
            return "kucoin";
        case Exchange::LIGHTER:
            return "lighter";
        case Exchange::LTP_BINANCE:
            return "ltpbinance";
        case Exchange::NADO:
            return "nado";
        case Exchange::OKEX:
            return "okex";
        case Exchange::ORANGEX:
            return "orangex";
        case Exchange::PARADEX:
            return "paradex";
        case Exchange::PHEMEX:
            return "phemex";
        case Exchange::TOOBIT:
            return "toobit";
        case Exchange::WEEX:
            return "weex";
        case Exchange::XT:
            return "xt";
        default:
            return "unknown";
    }
}

inline Exchange exchange_from_text(std::string_view text) {
    if (text == "apex") {
        return Exchange::APEX;
    } else if (text == "aster") {
        return Exchange::ASTER;
    } else if (text == "backpack") {
        return Exchange::BACKPACK;
    } else if (text == "binance") {
        return Exchange::BINANCE;
    } else if (text == "bingx") {
        return Exchange::BINGX;
    } else if (text == "bitget") {
        return Exchange::BITGET;
    } else if (text == "bitmart") {
        return Exchange::BITMART;
    } else if (text == "bitunix") {
        return Exchange::BITUNIX;
    } else if (text == "bybit") {
        return Exchange::BYBIT;
    } else if (text == "coinex") {
        return Exchange::COINEX;
    } else if (text == "crossexgate") {
        return Exchange::CROSSEX_GATE;
    } else if (text == "dexless") {
        return Exchange::DEXLESS;
    } else if (text == "edgex") {
        return Exchange::EDGEX;
    } else if (text == "gate") {
        return Exchange::GATE;
    } else if (text == "hbg") {
        return Exchange::HBG;
    } else if (text == "hyperliquid") {
        return Exchange::HYPERLIQUID;
    } else if (text == "kucoin") {
        return Exchange::KUCOIN;
    } else if (text == "lighter") {
        return Exchange::LIGHTER;
    } else if (text == "LTPBINANCE" || text == "ltpbinance") {
        return Exchange::LTP_BINANCE;
    } else if (text == "nado") {
        return Exchange::NADO;
    } else if (text == "okx" || text == "okex" || text == "okexv5") {
        return Exchange::OKEX;
    } else if (text == "orangex") {
        return Exchange::ORANGEX;
    } else if (text == "paradex") {
        return Exchange::PARADEX;
    } else if (text == "phemex") {
        return Exchange::PHEMEX;
    } else if (text == "toobit") {
        return Exchange::TOOBIT;
    } else if (text == "weex") {
        return Exchange::WEEX;
    } else if (text == "xt") {
        return Exchange::XT;
    } else {
        return Exchange::UNKNOWN;
    }
}

// 根据小数位数获取步长
inline double get_step_by_decimals(int64_t precision) {
    static const double powers_of_10[] = {1e0, 1e-1, 1e-2, 1e-3, 1e-4, 1e-5, 1e-6, 1e-7, 1e-8, 1e-9};
    if (precision >= 0 && precision <= 9)
        return powers_of_10[precision];
    else
        return pow(10.0, -static_cast<double>(precision));
}

// 根据步长获取小数位数
inline int get_decimals_by_step(double step_size) {
    if (step_size <= 0.0)
        return 0;
    int decimals = static_cast<int>(std::round(-std::log10(step_size)));
    return std::max(0, decimals);
}

inline double str_to_float(const std::string& str) { return (!str.empty() ? std::stod(str) : 0); }

inline double str_to_float(std::string_view sv) { return str_to_float(std::string(sv)); }

// 根据步长调整精度
inline double adjust_precision(double value, double step) {
    // 获取小数位数
    int step_decimals = 0;
    double temp = step;
    while (std::abs(temp - std::round(temp)) > 1e-12 && step_decimals < 10) {
        temp *= 10;
        step_decimals++;
    }

    // 转换为整数运算
    double multiplier = std::pow(10.0, step_decimals);
    long long value_int = static_cast<long long>(std::round(value * multiplier));
    long long step_int = static_cast<long long>(std::round(step * multiplier));

    long long result_int = (value_int / step_int) * step_int;
    return result_int / multiplier;
}

inline double calc_decimal_sbe(int64_t mantissa, int8_t exponent) {
    if (mantissa == 0) {
        return 0.0;
    }

    switch (exponent) {
        case -5:
            return static_cast<double>(mantissa) * 1e-5;
        case -4:
            return static_cast<double>(mantissa) * 1e-4;
        case -3:
            return static_cast<double>(mantissa) * 1e-3;
        case -2:
            return static_cast<double>(mantissa) * 1e-2;
        case -1:
            return static_cast<double>(mantissa) * 1e-1;
        case 0:
            return static_cast<double>(mantissa);
        case 1:
            return static_cast<double>(mantissa) * 1e1;
        case 2:
            return static_cast<double>(mantissa) * 1e2;
        case 3:
            return static_cast<double>(mantissa) * 1e3;
        case 4:
            return static_cast<double>(mantissa) * 1e4;
        case 5:
            return static_cast<double>(mantissa) * 1e5;
        default:
            return static_cast<double>(mantissa) * std::pow(10.0, exponent);
    }
}

inline bool is_zero(double a) { return std::fabs(a) < 1e-9; }

inline bool compare_currency(const Currency& str1, const Currency& str2) {
    if (str1.length() != str2.length()) {
        return false;
    }

    for (size_t i = 0; i < str1.length(); ++i) {
        if (std::tolower(static_cast<unsigned char>(str1[i])) != std::tolower(static_cast<unsigned char>(str2[i]))) {
            return false;
        }
    }
    return true;
}

inline std::string to_lower_str(const std::string& data) {
    std::string str{};
    str.reserve(data.size());
    for (unsigned char c : data) {
        str += (c >= 'A' && c <= 'Z') ? (char)(c | 0x20) : (char)c;
    }
    return str;
}

inline std::string to_upper_str(const std::string& data) {
    std::string str{};
    str.reserve(data.size());
    for (unsigned char c : data) {
        str += (c >= 'a' && c <= 'z') ? (char)(c & ~0x20) : (char)c;
    }
    return str;
}

inline std::string make_compact_pair(const std::string& pair) {
    std::string result{};
    result.reserve(pair.size());
    for (char c : pair) {
        if (c == '_' || c == '-' || c == '/' || c == '.') {
            continue;
        }
        result += c;
    }
    return result;
}

inline std::string make_underscore_pair(const std::string& pair) {
    std::string result{};
    result.reserve(pair.size());
    for (char c : pair) {
        if (c == '-' || c == '/') {
            result += '_';
        } else {
            result += c;
        }
    }
    return result;
}

inline std::string make_hyphen_pair(const std::string& pair) {
    std::string result{};
    for (char c : pair) {
        if (c == '_' || c == '/') {
            result += '-';
        } else {
            result += c;
        }
    }
    return result;
}

inline std::string add_dash_before_quote(const std::string& symbol, const Currency& currency) {
    std::vector<std::string> quotes = {"USDT", "usdt", "USDTM", "USD", "usd", "USDC", "usdc"};
    for (const auto& quote : quotes) {
        if (symbol.length() > quote.length()) {
            // 检查是否以这些货币结尾
            if (symbol.compare(symbol.length() - quote.length(), quote.length(), quote) == 0) {
                std::string base = symbol.substr(0, symbol.length() - quote.length());
                return base + "-" + quote;
            }
        }
    }

    return symbol; // 没有匹配的计价货币，返回原字符串
}

inline std::string get_random_str(size_t length) {
    const std::string charset = "0123456789"
                                "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                                "abcdefghijklmnopqrstuvwxyz";

    std::random_device rd;
    std::mt19937 generator(rd());
    std::uniform_int_distribution<> distribution(0, charset.size() - 1);

    std::string result;
    result.reserve(length);
    for (size_t i = 0; i < length; ++i) {
        result.push_back(charset[distribution(generator)]);
    }
    return result;
}

inline void bind_cpu(int cpu_id) {
    cpu_set_t mask;
    CPU_ZERO(&mask);
    CPU_SET(cpu_id, &mask);
    if (sched_setaffinity(0, sizeof(mask), &mask) == -1) {
        throw std::system_error(errno, std::system_category(), "sched_setaffinity failed");
    }
}

inline std::string to_exchange_pair(Exchange ex, const std::string& pair) {
    std::string symbol{};
    switch (ex) {
        case Exchange::BINGX:
        case Exchange::HBG:
        case Exchange::APEX:
            symbol = to_upper_str(pair); // 形式一致，大写形式
            break;
        case Exchange::BINANCE:
        case Exchange::BITGET:
        case Exchange::BITMART:
        case Exchange::BYBIT:
        case Exchange::COINEX:
        case Exchange::PHEMEX:
        case Exchange::ASTER:
        case Exchange::EDGEX:
        case Exchange::BITUNIX:
        case Exchange::WEEX:
            symbol = to_upper_str(make_compact_pair(pair)); // 无分隔符，大写形式
            break;
        case Exchange::XT:
            symbol = to_lower_str(make_underscore_pair(pair)); // 下划线分隔符，小写形式
            break;
        case Exchange::GATE:
            symbol = to_upper_str(make_underscore_pair(pair)); // 下划线分隔符，大写形式
            break;
        case Exchange::CROSSEX_GATE:
            symbol = to_upper_str(make_underscore_pair(pair)); // 下划线分隔符，大写形式
            break;
        case Exchange::LTP_BINANCE:
            symbol = to_upper_str(make_underscore_pair(pair));
            symbol = "BINANCE_PERP_" + symbol; // 加上前缀 "BINANCE_PERP_"
            break;
        case Exchange::OKEX:
            symbol = to_upper_str(pair) + "-SWAP"; // 大写形式，加上后缀"-SWAP"
            break;
        case Exchange::KUCOIN: {
            symbol = to_upper_str(make_compact_pair(pair)).append("M"); // 转成紧凑型, 加上后缀"M"
            size_t pos = symbol.find("BTC");
            if (pos != std::string::npos) {
                symbol.replace(pos, 3, "XBT"); // BTC特殊处理
            }
        } break;
        case Exchange::BACKPACK:
            symbol = to_upper_str(make_underscore_pair(pair));
            symbol.pop_back();  // USDT --> USDC
            symbol += "C_PERP"; // 大写形式，加上后缀"_PERP"
            break;
        case Exchange::PARADEX:
            symbol = to_upper_str(pair);
            symbol.pop_back(); // USDT --> USD
            symbol += "-PERP"; // 大写形式，加上后缀"-PERP"
            break;
        case Exchange::DEXLESS:
            symbol = "PERP_" + to_upper_str(make_underscore_pair(pair));
            symbol.pop_back(); // USDT --> USDC
            symbol += "C";
            break;
        case Exchange::TOOBIT:
            symbol = to_upper_str(pair.substr(0, pair.size() - 5)) + "-SWAP-USDT"; // 大写形式，去掉后缀"-USDT"
            break;
        case Exchange::ORANGEX:
            symbol = to_upper_str(pair) + "-PERPETUAL"; // 大写形式，加上后缀"-PERPETUAL"
            break;
        default:
            break;
    }
    return symbol;
}

inline std::string to_infra_pair(Exchange ex, const std::string& pair) {
    std::string symbol{};
    switch (ex) {
        case Exchange::BINGX:
        case Exchange::HBG:
        case Exchange::APEX:
            symbol = pair; // 形式一致
            break;
        case Exchange::BINANCE:
        case Exchange::BITGET:
        case Exchange::BITMART:
        case Exchange::BYBIT:
        case Exchange::COINEX:
        case Exchange::PHEMEX:
        case Exchange::ASTER:
        case Exchange::EDGEX:
        case Exchange::BITUNIX:
        case Exchange::WEEX:
            symbol = add_dash_before_quote(pair, "USDT"); // 在计价货币之前添加短横线
            break;
        case Exchange::XT:
        case Exchange::GATE:
            symbol = make_hyphen_pair(pair); // 原分隔符改成短横线
            break;
        case Exchange::CROSSEX_GATE:
            symbol = make_hyphen_pair(pair); // 原分隔符改成短横线
            break;
        case Exchange::LTP_BINANCE:
            symbol = make_hyphen_pair(pair); // 原分隔符改成短横线
            symbol = symbol.substr(13);      // 去掉前缀 "BINANCE_PERP_"
            break;
        case Exchange::OKEX:
            if (pair.size() > 5 && pair.substr(pair.size() - 5) == "-SWAP") {
                symbol = pair.substr(0, pair.size() - 5); // 去掉后缀 "-SWAP"
            } else {
                symbol = pair;
            }
            break;
        case Exchange::KUCOIN: {
            // 紧凑型添加分隔符, 去掉后缀"M"
            symbol = add_dash_before_quote(pair, "USDTM");
            symbol.pop_back();
            size_t pos = symbol.find("XBT");
            if (pos != std::string::npos) {
                symbol.replace(pos, 3, "BTC");
            }
        } break;
        case Exchange::BACKPACK:
            symbol = pair.substr(0, pair.size() - 5); // 去掉后缀 "_PERP"
            symbol.pop_back();
            symbol += "T"; // USDC --> USDT
            symbol = make_hyphen_pair(symbol);
            break;
        case Exchange::PARADEX:
            symbol = pair.substr(0, pair.size() - 5); // 去掉后缀 "-PERP"
            symbol += "T";                            // USD --> USDT
            break;
        case Exchange::NADO:
            symbol = pair.substr(0, pair.size() - 5); // 去掉后缀 "-PERP"
            symbol += "-USDT";                        // 添加后缀
            break;
        case Exchange::HYPERLIQUID:
        case Exchange::LIGHTER:
            symbol = pair + "-USDT"; // 添加后缀
            break;
        case Exchange::DEXLESS: {
            std::string symbol_sub = pair.compare(0, 5, "PERP_") == 0 ? pair.substr(5) : pair;
            symbol = make_hyphen_pair(symbol_sub);
            symbol.pop_back();
            symbol += "T"; // USDC --> USDT
            break;
        }
        case Exchange::TOOBIT:
            symbol = pair.substr(0, pair.size() - 10); // 去掉后缀 "-SWAP-USDT"
            symbol += "-USDT";                         // 添加后缀
            break;
        case Exchange::ORANGEX:
            symbol = pair.substr(0, pair.size() - 10); // 去掉后缀 "-PERPETUAL"
            break;
        default:
            symbol = pair;
            break;
    }
    return to_lower_str(symbol); // 统一转为小写形式
}

inline std::string to_infra_pair(Exchange ex, std::string_view sv) { return to_infra_pair(ex, std::string(sv)); }

} // namespace infra
