## qtx产品使用特点
* 提供机器内网ip，配置aws vpc，路由表等
* SDK采用纯C编写，和asio不兼容
* SDK采用多线程架构，需要大机器才能充分发挥性能

## 行情测试
## 系统配置(aws，ubuntu 24.04，c8a.large 2core4g，ap-northeast-1c)
- **CPU频率**: 2.600 GHz
- **测试说明**: 只订阅`btc-usdt`行情

### infra测试程序
| 指标 | 平均值 | 最小值 | 最大值 | 标准差 | P50 | P95 | P99 | P99.9 | 单位 |
|------|-------|-------|--------|-------|-----|-----|-----|-----|-----|
| Exchange To Local | 2.88 | 1.00 | 220.00 | 4.37 | 2.00 | 9.00 | 21.00 | 54.00 | ms |
| Ob Recv To Parsed | 0.53 | 0.46 | 287.87 | 0.20 | 0.52 | 0.55 | 0.57 | 3.81 | us |
| Send To Sent | 30.34 | 6.86 | 66.72 | 6.20 | 30.79 | 39.11 | 43.15 | 57.79 | us |
| Sent To Order Ack | 3.62 | 2.79 | 3096.21 | 31.68 | 3.09 | 3.47 | 5.16 | 22.77 | ms |

### qtx快速行情示例程序（改成合约行情，运行24小时）
```
========================================
    Latency Statistics Report
========================================
Ticker messages processed:
  Valid samples:           500000
  Filtered (negative):     0 (clock skew)
  Filtered (>100ms):       0 (stale data)
  Filtered (no timestamp): 0

Non-ticker messages (not included in latency):
  L2 Depth                  3206679
  Mark Price                86399
  Trade                     3270453
  Aggregate Trade           1213259
  24h Ticker                43018
  Open Interest             17279
  Long-Short Ratio          4320
  Force Order               975

Duration: 86400 seconds
Message rate: 5.8 msg/sec (ticker only)

Latency Percentiles (milliseconds):
  p1:  1.197 ms
  p25: 1.670 ms
  p50: 1.851 ms (median)
  p75: 2.149 ms
  p99: 2.418 ms
========================================

Example completed. Unsubscribing...
Unsubscribed from binance-perp:btcusdt
```

## 使用特点
* qtx采用自有协议对Binance数据做了封装，如果接入，需要修改大量代码做适配


## 下单测试
## 系统配置(aws，ubuntu 24.04，c8a.large 2core4g，ap-northeast-1c)
- **CPU频率**: 2.600 GHz
- **测试说明**: 只订阅`btc-usdt`行情，每5s下一单
- **测试要点**: 使用不同账号做对比测试，在一台机器上同时运行两个测试程序，并绑定不同的核 

## case 2（2026.05.20）（运行时间约24小时）
### infra测试程序
| 指标 | 平均值 | 最小值 | 最大值 | 标准差 | P50 | P95 | P99 | P99.9 | 单位 |
|------|-------|-------|--------|-------|-----|-----|-----|-----|-----|
| Exchange To Local | 1.64 | 1.00 | 228.00 | 1.32 | 2.00 | 2.00 | 5.00 | 14.00 | ms |
| Ob Recv To Parsed | 0.53 | 0.46 | 71.27 | 0.11 | 0.53 | 0.56 | 0.58 | 0.78 | us |
| Send To Sent | 32.57 | 5.68 | 116.66 | 9.00 | 35.66 | 41.87 | 44.75 | 60.19 | us |
| Sent To Order Ack | 3.16 | 2.53 | 1247.30 | 10.91 | 2.90 | 3.29 | 4.88 | 21.05 | ms |

### qtx快速下单，sytus_order
| 指标 | 平均值 | 最小值 | 最大值 | 标准差 | P50 | P95 | P99 | P99.9 | 单位 |
|------|-------|-------|--------|-------|-----|-----|-----|-----|-----|
| Exchange latency | 3.94 | 2.78 | 4254.21 | 44.82 | 3.15 | 3.57 | 4.74 | 25.58 | ms |

## 使用特点
* qtx提供SDK
* SDK内部采用C + select方式进行网络通信，属于多线程架构，和boost asio兼容性差