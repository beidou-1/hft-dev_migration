#pragma once

#include <cstdlib>
#include <iostream>
#include <string>
#include <filesystem>
#include "common/logger.h"

using namespace infra;

// 获取测试交易所类型
#pragma GCC diagnostic push
#pragma GCC diagnostic error "-Wreturn-type"
inline Exchange get_test_exchange_type() {
#ifdef ENABLED_APEX
    return Exchange::APEX;
#endif
#ifdef ENABLED_ASTER
    return Exchange::ASTER;
#endif
#ifdef ENABLED_BACKPACK
    return Exchange::BACKPACK;
#endif
#ifdef ENABLED_BINANCE
    return Exchange::BINANCE;
#endif
#ifdef ENABLED_LTPBINANCE
    return Exchange::LTP_BINANCE;
#endif
#ifdef ENABLED_BINGX
    return Exchange::BINGX;
#endif
#ifdef ENABLED_BITGET
    return Exchange::BITGET;
#endif
#ifdef ENABLED_BITMART
    return Exchange::BITMART;
#endif
#ifdef ENABLED_BITUNIX
    return Exchange::BITUNIX;
#endif
#ifdef ENABLED_BYBIT
    return Exchange::BYBIT;
#endif
#ifdef ENABLED_COINEX
    return Exchange::COINEX;
#endif
#ifdef ENABLED_CROSSEX_GATE
    return Exchange::CROSSEX_GATE;
#endif
#ifdef ENABLED_DEXLESS
    return Exchange::DEXLESS;
#endif
#ifdef ENABLED_EDGEX
    return Exchange::EDGEX;
#endif
#ifdef ENABLED_GATE
    return Exchange::GATE;
#endif
#ifdef ENABLED_HBG
    return Exchange::HBG;
#endif
#ifdef ENABLED_HYPERLIQUID
    return Exchange::HYPERLIQUID;
#endif
#ifdef ENABLED_KUCOIN
    return Exchange::KUCOIN;
#endif
#ifdef ENABLED_LIGHTER
    return Exchange::LIGHTER;
#endif
#ifdef ENABLED_NADO
    return Exchange::NADO;
#endif
#ifdef ENABLED_OKEX
    return Exchange::OKEX;
#endif
#ifdef ENABLED_ORANGEX
    return Exchange::ORANGEX;
#endif
#ifdef ENABLED_PARADEX
    return Exchange::PARADEX;
#endif
#ifdef ENABLED_PHEMEX
    return Exchange::PHEMEX;
#endif
#ifdef ENABLED_TOOBIT
    return Exchange::TOOBIT;
#endif
#ifdef ENABLED_VARIATIONAL
    return Exchange::VARIATIONAL;
#endif
#ifdef ENABLED_WEEX
    return Exchange::WEEX;
#endif
#ifdef ENABLED_XT
    return Exchange::XT;
#endif
}
#pragma GCC diagnostic pop

inline Symbols get_test_symbols() { return {"btc-usdt","eth-usdt", "xrp-usdt", "bnb-usdt","sol-usdt"}; } // 订阅行情
inline Symbol get_primary_test_symbol() { return Symbol{"xrp-usdt"}; }
inline Symbol get_trade_perf_test_symbol() { return Symbol{"bnb-usdt"}; }
inline Currency get_test_currency() { return Currency{"usdt"}; }

// 获取环境变量中的API密钥
inline AccountSecret get_api_credentials() {
    char* infra_api_key = std::getenv("INFRA_API_KEY");
    char* infra_api_secret = std::getenv("INFRA_API_SECRET");
    char* infra_api_phrase = std::getenv("INFRA_API_PHRASE");
    char* infra_wallet_address = std::getenv("INFRA_WALLET_ADDRESS");

    AccountSecret secret{.api_key = (infra_api_key) ? infra_api_key : "",
                         .api_secret = (infra_api_secret) ? infra_api_secret : "",
                         .api_phrase = (infra_api_phrase) ? infra_api_phrase : "",
                         .wallet_address = (infra_wallet_address) ? infra_wallet_address : ""};

// 自定义字段
#if defined(ENABLED_LIGHTER)
    secret.custom_info["api_key_index"] = "";
    secret.custom_info["api_token"] = "";
    secret.custom_info["account_id"] = "";
#endif
    return secret;
}

// 创建默认API配置
inline APIConfig create_default_config() {
    APIConfig config;
    config.name = get_test_exchange_type();
    config.account_type = AccountType::SWAP;
    config.address_type = AddressType::NORMAL;
    config.settle_unit = Settlement::USDT;
    config.account_mode = AccountMode::CLASSIC;
    return config;
}
