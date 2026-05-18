/**
 * @file factory.h
 * @brief 交易所工厂类
 * @details 实现工厂模式，用于创建不同交易所的客户端实例
 */

#pragma once

#include <iostream>
#include "client.h"
#include "structs.h"
#include "logger.h"

namespace infra {

/**
 * @class ExchangeFactory
 * @brief 交易所工厂类（单例模式）
 * @details 管理不同交易所的构建器，提供统一的创建接口
 */
class ExchangeFactory {
public:
    /// 交易所客户端构建器类型
    using Builder = std::function<SpExchangeClient(net::io_context&, ssl::context&, const AccountSecret&, APIConfig)>;

    /**
     * @brief 获取工厂单例
     * @return 工厂实例引用
     */
    static ExchangeFactory& instance() {
        static ExchangeFactory inst;
        return inst;
    }

    /**
     * @brief 注册交易所构建器
     * @param ex 交易所枚举值
     * @param builder 构建器函数
     */
    void register_exchange(Exchange ex, Builder builder) { registry_[ex] = std::move(builder); }

    /**
     * @brief 创建交易所客户端
     * @param ioc IO上下文
     * @param ssl_ctx SSL上下文
     * @param sec 账户密钥
     * @param config API配置
     * @return 交易所客户端智能指针，失败返回nullptr
     */
    SpExchangeClient create(net::io_context& ioc, ssl::context& ssl_ctx, const AccountSecret& sec, APIConfig config) {
        auto it = registry_.find(config.name);
        if (it == registry_.end()) {
            INFRA_LOG_ERROR("[infra] Exchange {} not found", exchange_to_text(config.name));
            return nullptr;
        }
        return it->second(ioc, ssl_ctx, sec, config);
    }

private:
    ExchangeFactory() = default;
    std::unordered_map<Exchange, Builder> registry_; ///< 交易所构建器注册表
};

/**
 * @def REGISTER_EXCHANGE
 * @brief 交易所注册宏
 * @param EnumValue 交易所枚举值
 * @param AccountClass 账户类名
 * @param MarketDataClass 行情数据类名
 * @param ExecutionClass 交易执行类名
 * @details 用法：REGISTER_EXCHANGE(BINANCE, BinanceAccount, BinanceMarketData, BinanceExecution)
 */
#define REGISTER_EXCHANGE(EnumValue, AccountClass, MarketDataClass, ExecutionClass)                                    \
    struct ExchangeRegistrar_##EnumValue {                                                                             \
        ExchangeRegistrar_##EnumValue() {                                                                              \
            infra::ExchangeFactory::instance().register_exchange(                                                      \
                infra::Exchange::EnumValue, [](net::io_context& ioc, ssl::context& ssl_ctx,                            \
                                               const infra::AccountSecret& sec, infra::APIConfig config) {             \
                    auto mkt = std::make_unique<infra::MarketDataClass>(ioc, ssl_ctx, sec, config);                    \
                    auto acc = std::make_unique<infra::AccountClass>(ioc, ssl_ctx, sec, config);                       \
                    auto exe = std::make_unique<infra::ExecutionClass>(ioc, ssl_ctx, sec, config);                     \
                    return std::make_shared<infra::ExchangeClient>(std::move(acc), std::move(mkt), std::move(exe));    \
                });                                                                                                    \
        }                                                                                                              \
    };                                                                                                                 \
    static ExchangeRegistrar_##EnumValue registrar_##EnumValue;
} // namespace infra

#ifdef ENABLED_APEX
#include "exchanges/apex/apex_account.h"
#include "exchanges/apex/apex_market_data.h"
#include "exchanges/apex/apex_execution.h"
REGISTER_EXCHANGE(APEX, ApexAccount, ApexMarketData, ApexExecution)
#endif

#ifdef ENABLED_ASTER
#include "exchanges/aster/aster_account.h"
#include "exchanges/aster/aster_market_data.h"
#include "exchanges/aster/aster_execution.h"
REGISTER_EXCHANGE(ASTER, AsterAccount, AsterMarketData, AsterExecution)
#endif

#ifdef ENABLED_BACKPACK
#include "exchanges/backpack/backpack_account.h"
#include "exchanges/backpack/backpack_market_data.h"
#include "exchanges/backpack/backpack_execution.h"
REGISTER_EXCHANGE(BACKPACK, BackpackAccount, BackpackMarketData, BackpackExecution)
#endif

#ifdef ENABLED_BINANCE
#include "exchanges/binance/binance_account.h"
#include "exchanges/binance/binance_market_data.h"
#include "exchanges/binance/binance_execution.h"
REGISTER_EXCHANGE(BINANCE, BinanceAccount, BinanceMarketData, BinanceExecution)
#endif

#ifdef ENABLED_LTPBINANCE
#include "exchanges/binance_ltp/binance_ltp_account.h"
#include "exchanges/binance_ltp/binance_ltp_market_data.h"
#include "exchanges/binance_ltp/binance_ltp_execution.h"
REGISTER_EXCHANGE(LTP_BINANCE, BinanceLtpAccount, BinanceLtpMarketData, BinanceLtpExecution)
#endif

#ifdef ENABLED_BINGX
#include "exchanges/bingx/bingx_account.h"
#include "exchanges/bingx/bingx_market_data.h"
#include "exchanges/bingx/bingx_execution.h"
REGISTER_EXCHANGE(BINGX, BingxAccount, BingxMarketData, BingxExecution)
#endif

#ifdef ENABLED_BITGET
#include "exchanges/bitget/bitget_account.h"
#include "exchanges/bitget/bitget_market_data.h"
#include "exchanges/bitget/bitget_execution.h"
REGISTER_EXCHANGE(BITGET, BitgetAccount, BitgetMarketData, BitgetExecution)
#endif

#ifdef ENABLED_BITMART
#include "exchanges/bitmart/bitmart_account.h"
#include "exchanges/bitmart/bitmart_market_data.h"
#include "exchanges/bitmart/bitmart_execution.h"
REGISTER_EXCHANGE(BITMART, BitmartAccount, BitmartMarketData, BitmartExecution)
#endif

#ifdef ENABLED_BITUNIX
#include "exchanges/bitunix/bitunix_account.h"
#include "exchanges/bitunix/bitunix_market_data.h"
#include "exchanges/bitunix/bitunix_execution.h"
REGISTER_EXCHANGE(BITUNIX, BitunixAccount, BitunixMarketData, BitunixExecution)
#endif

#ifdef ENABLED_BYBIT
#include "exchanges/bybit/bybit_account.h"
#include "exchanges/bybit/bybit_market_data.h"
#include "exchanges/bybit/bybit_execution.h"
REGISTER_EXCHANGE(BYBIT, BybitAccount, BybitMarketData, BybitExecution)
#endif

#ifdef ENABLED_COINEX
#include "exchanges/coinex/coinex_account.h"
#include "exchanges/coinex/coinex_market_data.h"
#include "exchanges/coinex/coinex_execution.h"
REGISTER_EXCHANGE(COINEX, CoinexAccount, CoinexMarketData, CoinexExecution)
#endif

#ifdef ENABLED_DEXLESS
#include "exchanges/dexless/dexless_account.h"
#include "exchanges/dexless/dexless_market_data.h"
#include "exchanges/dexless/dexless_execution.h"
REGISTER_EXCHANGE(DEXLESS, DexlessAccount, DexlessMarketData, DexlessExecution)
#endif

#ifdef ENABLED_EDGEX
#include "exchanges/edgex/edgex_account.h"
#include "exchanges/edgex/edgex_market_data.h"
#include "exchanges/edgex/edgex_execution.h"
REGISTER_EXCHANGE(EDGEX, EdgexAccount, EdgexMarketData, EdgexExecution)
#endif

#ifdef ENABLED_GATE
#include "exchanges/gate/gate_account.h"
#include "exchanges/gate/gate_market_data.h"
#include "exchanges/gate/gate_execution.h"
REGISTER_EXCHANGE(GATE, GateAccount, GateMarketData, GateExecution)
#endif

#ifdef ENABLED_CROSSEX_GATE
#include "exchanges/crossex_gate/crossex_gate_account.h"
#include "exchanges/crossex_gate/crossex_gate_market_data.h"
#include "exchanges/crossex_gate/crossex_gate_execution.h"
REGISTER_EXCHANGE(CROSSEX_GATE, CrossexGateAccount, CrossexGateMarketData, CrossexGateExecution)
#endif

#ifdef ENABLED_HBG
#include "exchanges/hbg/hbg_account.h"
#include "exchanges/hbg/hbg_market_data.h"
#include "exchanges/hbg/hbg_execution.h"
REGISTER_EXCHANGE(HBG, HbgAccount, HbgMarketData, HbgExecution)
#endif

#ifdef ENABLED_HYPERLIQUID
#include "exchanges/hyperliquid/hyperliquid_account.h"
#include "exchanges/hyperliquid/hyperliquid_market_data.h"
#include "exchanges/hyperliquid/hyperliquid_execution.h"
REGISTER_EXCHANGE(HYPERLIQUID, HyperliquidAccount, HyperliquidMarketData, HyperliquidExecution)
#endif

#ifdef ENABLED_KUCOIN
#include "exchanges/kucoin/kucoin_account.h"
#include "exchanges/kucoin/kucoin_market_data.h"
#include "exchanges/kucoin/kucoin_execution.h"
REGISTER_EXCHANGE(KUCOIN, KucoinAccount, KucoinMarketData, KucoinExecution)
#endif

#ifdef ENABLED_LIGHTER
#include "exchanges/lighter/lighter_account.h"
#include "exchanges/lighter/lighter_market_data.h"
#include "exchanges/lighter/lighter_execution.h"
REGISTER_EXCHANGE(LIGHTER, LighterAccount, LighterMarketData, LighterExecution)
#endif

#ifdef ENABLED_NADO
#include "exchanges/nado/nado_account.h"
#include "exchanges/nado/nado_market_data.h"
#include "exchanges/nado/nado_execution.h"
REGISTER_EXCHANGE(NADO, NadoAccount, NadoMarketData, NadoExecution)
#endif

#ifdef ENABLED_OKEX
#include "exchanges/okex/okex_account.h"
#include "exchanges/okex/okex_market_data.h"
#include "exchanges/okex/okex_execution.h"
REGISTER_EXCHANGE(OKEX, OkxAccount, OkxMarketData, OkxExecution)
#endif

#ifdef ENABLED_ORANGEX
#include "exchanges/orangex/orangex_account.h"
#include "exchanges/orangex/orangex_market_data.h"
#include "exchanges/orangex/orangex_execution.h"
REGISTER_EXCHANGE(ORANGEX, OrangexAccount, OrangexMarketData, OrangexExecution)
#endif

#ifdef ENABLED_PARADEX
#include "exchanges/paradex/paradex_account.h"
#include "exchanges/paradex/paradex_market_data.h"
#include "exchanges/paradex/paradex_execution.h"
REGISTER_EXCHANGE(PARADEX, ParadexAccount, ParadexMarketData, ParadexExecution)
#endif

#ifdef ENABLED_PHEMEX
#include "exchanges/phemex/phemex_account.h"
#include "exchanges/phemex/phemex_market_data.h"
#include "exchanges/phemex/phemex_execution.h"
REGISTER_EXCHANGE(PHEMEX, PhemexAccount, PhemexMarketData, PhemexExecution)
#endif

#ifdef ENABLED_TOOBIT
#include "exchanges/toobit/toobit_account.h"
#include "exchanges/toobit/toobit_market_data.h"
#include "exchanges/toobit/toobit_execution.h"
REGISTER_EXCHANGE(TOOBIT, ToobitAccount, ToobitMarketData, ToobitExecution)
#endif

#ifdef ENABLED_WEEX
#include "exchanges/weex/weex_account.h"
#include "exchanges/weex/weex_market_data.h"
#include "exchanges/weex/weex_execution.h"
REGISTER_EXCHANGE(WEEX, WeexAccount, WeexMarketData, WeexExecution)
#endif

#ifdef ENABLED_XT
#include "exchanges/xt/xt_account.h"
#include "exchanges/xt/xt_market_data.h"
#include "exchanges/xt/xt_execution.h"
REGISTER_EXCHANGE(XT, XtAccount, XtMarketData, XtExecution)
#endif
