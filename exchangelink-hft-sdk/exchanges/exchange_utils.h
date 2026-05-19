#pragma once

#include <string>
#include <string_view>
#include <unordered_map>
#include <cmath>
#include <simdjson/simdjson.h>
#include "common/types.h"

namespace infra {
// json解析宏
#define PARSE_JSON(json_str, doc)                                                                                      \
    simdjson::padded_string simd_str(json_str);                                                                        \
    simdjson::dom::parser parser;                                                                                      \
    simdjson::dom::element doc = parser.parse(simd_str)

// 配置信息映射表类型
using UMConfig = std::unordered_map<std::string, std::string>;
using UMExchangeConfig = std::unordered_map<std::string, UMConfig>;

// NOTE：通用的配置字段名
static const std::string REST_HOST = "rest_host";
static const std::string REST_PORT = "rest_port";
static const std::string WSS_PUBLIC_HOST = "wss_public_host";
static const std::string WSS_PUBLIC_PATH = "wss_public_path";
static const std::string WSS_PRIVATE_HOST = "wss_private_host";
static const std::string WSS_PRIVATE_PATH = "wss_private_path";
static const std::string WSS_TRADE_HOST = "wss_trade_host";
static const std::string WSS_TRADE_PATH = "wss_trade_path";
static const std::string WSS_PORT = "wss_port";

static const std::string PAIRS_INFO_PATH = "pairs_info_path";
static const std::string FUNDING_FEE_PATH = "funding_fee_path";
static const std::string BALANCE_PATH = "balance_path";
static const std::string POSITION_PATH = "position_path";
static const std::string LEVERAGE_PATH = "leverage_path";
static const std::string ORDER_PATH_PATH = "order_path";
static const std::string QUERY_ORDER_PATH_PATH = "query_order_path";
static const std::string PLACE_ORDER_PATH_PATH = "place_order_path";
static const std::string CANCEL_ORDER_PATH_PATH = "cancel_order_path";

static const std::string LISTEN_KEY_PATH = "listen_key_path";

} // namespace infra
