# exchangelink-hft-sdk

交易所 SDK（ExchangeLink HFT SDK），提供标准化的账户、行情和交易接口

## 项目介绍

* infra项目采用单线程异步模型，大多数接口都要求提供相应的回调函数
* 基于Boost Asio实现了支持高并发的REST和WebSocket的客户端

## 功能介绍

- **账户相关**: 支持查询资产、查询持仓、调整合约杠杆、设置保证金模式、设置仓位模式
- **行情数据**: 支持订阅不同档位的订单簿、获取交易对信息、获取资金费率
- **交易模块**: 支持查询订单、REST下单&撤单（部分所支持WebSocket形式）、订阅订单变动和成交推送

## 限制说明
- 当前接入的所仅支持U本位永续合约，如需支持其他类型，请提需求

## 交易所支持情况

### CEX
- **Binance**：已支持，[Binance接口说明和测试报告](./exchanges/binance/README.md)
- **Bingx**：已支持，[Bingx接口说明和测试报告](./exchanges/bingx/README.md)
- **Bitget**：已支持，[Bitget接口说明和测试报告](./exchanges/bitget/README.md)
- **Bitmart**：已支持，[Bitmart接口说明和测试报告](./exchanges/bitmart/README.md)
- **Bitunix**：已支持，[Bitunix接口说明和测试报告](./exchanges/bitunix/README.md)
- **Bybit**：已支持，[Bybit接口说明和测试报告](./exchanges/bybit/README.md)
- **Coinex**：已支持，[Coinex接口说明和测试报告](./exchanges/coinex/README.md)
- **Gate**：已支持，[Gate接口说明和测试报告](./exchanges/gate/README.md)
- **Hbg**：已支持，[Hbg接口说明和测试报告](./exchanges/hbg/README.md)
- **Kucoin**：已支持，[Kucoin接口说明和测试报告](./exchanges/kucoin/README.md)
- **Ltp_Binance**：已支持，[Ltp_Binance接口说明和测试报告](./exchanges/binance_ltp/README.md)
- **Okex**：已支持，[Okex接口说明和测试报告](./exchanges/okx/README.md)
- **Orangex**：已支持，[Orangex接口说明和测试报告](./exchanges/orangex/README.md)
- **Phemex**：已支持，[Phemex接口说明和测试报告](./exchanges/phemex/README.md)
- **Toobit**：已支持，[Toobit接口说明和测试报告](./exchanges/toobit/README.md)
- **Weex**：已支持，[Weex接口说明和测试报告](./exchanges/weex/README.md)
- **Xt**：已支持，[Xt接口说明和测试报告](./exchanges/xt/README.md)
- **Crossex_Gate**：已支持，[Crossex_Gate接口说明和测试报告](./exchanges/crossex_gate/README.md)

### DEX
- **Apex**：已支持，[Apex接口说明和测试报告](./exchanges/apex/README.md)
- **Aster**：已支持，[Aster接口说明和测试报告](./exchanges/aster/README.md)
- **Backpack**：已支持，[Backpack接口说明和测试报告](./exchanges/backpack/README.md)
- **Dexless**：已支持，[Dexless接口说明和测试报告](./exchanges/dexless/README.md)
- **Edgex**：已支持，[Edgex接口说明和测试报告](./exchanges/edgex/README.md)
- **Hyperliquid**：已支持，[Hyperliquid接口说明和测试报告](./exchanges/hyperliquid/README.md)
- **Lighter**：已支持，[Lighter接口说明和测试报告](./exchanges/lighter/README.md)
- **Nado**：已支持，[Nado接口说明和测试报告](./exchanges/nado/README.md)
- **Paradex**：已支持，[Paradex接口说明和测试报告](./exchanges/paradex/README.md)


## 快速开始

### 示例代码
```cpp
#include "common/factory.h"
using namespace infra;

// 创建交易所客户端
net::io_context ioc;
ssl::context ssl_ctx(ssl::context::tlsv12_client);

APIConfig config{
    .name = Exchange::BINANCE,
    .account_type = AccountType::SWAP,
    .address_type = AddressType::NORMAL,
    .settle_unit = Settlement::USDT
};

AccountSecret secret{/* API keys */};
auto client = ExchangeFactory::instance().create(ioc, ssl_ctx, secret, config);

// 初始化
bool res = client->initialize();

// 订阅1档行情
Symbols pairs = {"btc-usdt",  "eth-usdt"};
client->subscribe_orderbook(pairs, 1，callback);

// Rest下单
auto order = std::make_shared<Order>();
client->place_order_rest(order, callback);
```

### 使用说明
* common/client.h 对infra所有接口都做了功能和参数说明

* symbol格式说明：infra所有接口的参数，凡涉及symbol的都要求传固定格式`base-quote`（大小写均可），调用infra接口获取到的信息中，symbol同样是该种格式
