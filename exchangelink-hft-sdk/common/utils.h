/**
 * @file utils.h
 * @brief 工具函数集
 */

#pragma once

#include <string>
#include <string_view>
#include <vector>
#include <map>
#include "types.h"

namespace infra {

// 时间函数
Timestamp time_get_now_sec() noexcept;
Timestamp time_get_now_milli() noexcept;
Timestamp time_get_now_micro() noexcept;
Timestamp time_get_now_nano() noexcept;

std::string time_get_now_str();
std::string time_milli_to_iso(Timestamp milli);
Timestamp time_iso_to_milli(const char* text);

// rdtsc 时间函数（CPU周期级精度，适合HFT延迟统计）
inline uint64_t rdtsc() noexcept {
    uint32_t lo, hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

// 带序列化的 rdtscp，防止乱序执行，适合测量终点
inline uint64_t rdtscp() noexcept {
    uint32_t lo, hi, aux;
    __asm__ __volatile__("rdtscp" : "=a"(lo), "=d"(hi), "=c"(aux));
    return ((uint64_t)hi << 32) | lo;
}

void tsc_calibrate() noexcept;
uint64_t tsc_to_ns(uint64_t cycles) noexcept;
double get_cpu_freq_ghz() noexcept;

// 枚举转字符串函数
std::string to_string(Errno err);
std::string to_string(AccountType t);
std::string to_string(AddressType t);
std::string to_string(Settlement t);
std::string to_string(MarginMode m);
std::string to_string(PositionMode m);
std::string to_string(OrderSide s);
std::string to_string(OrderStatus s);
std::string to_string(OrderTIF t);
std::string to_string(OrderType t);

// 字符串转枚举函数
MarginMode to_margin_mode(std::string_view sv);
OrderStatus to_order_status(std::string_view sv);
OrderTIF to_order_tif(std::string_view sv);
OrderType to_order_type(std::string_view sv);
OrderSide to_order_side(std::string_view side, std::string_view position_side);

const char* order_side_to_text(OrderSide side);
OrderSide order_side_from_text(std::string_view side_text);
const char* allowence_to_text(OrderSide side);
OrderSide allowence_from_text(std::string_view side_text);
const char* exchange_to_text(Exchange exchange);
Exchange exchange_from_text(std::string_view text);

// 数值转换函数
double get_step_by_decimals(int64_t precision);
int get_decimals_by_step(double step_size);
double str_to_float(const std::string& str);
double str_to_float(std::string_view sv);
double adjust_precision(double value, double step);
double calc_decimal_sbe(int64_t mantissa, int8_t exponent);
bool is_zero(double a);

// 字符串处理函数
bool compare_currency(const Currency& str1, const Currency& str2);
std::string to_lower_str(const std::string& data);
std::string to_upper_str(const std::string& data);
std::string make_compact_pair(const std::string& pair);
std::string make_underscore_pair(const std::string& pair);
std::string make_hyphen_pair(const std::string& pair);
std::string add_dash_before_quote(const std::string& symbol, const Currency& currency);
std::string get_random_str(size_t length);

void bind_cpu(int cpu_id);

// pair转换函数
std::string to_exchange_pair(Exchange ex, const std::string& pair);
std::string to_infra_pair(Exchange ex, const std::string& pair);
std::string to_infra_pair(Exchange ex, std::string_view sv);
} // namespace infra

#include "utils-inl.h"
