# edgex Exchange

交易所测试报告和接口说明

## 测试报告

### 服务器
- **地理位置**: 未知

**典型地区测试结果**:
| 东京 | 新加坡 | 香港 | 弗吉尼亚 | 爱尔兰 |
|------|-------|------|---------|-------|
| 138 | 65 | 110 | 314 | 259 |

### 测试机器
- **配置信息**: AWS，EC2，亚太地区-新加坡(ap-southeast-1)


### a.网络延迟测试
**测试说明**: REST请求延迟是使用curl命令请求公共接口，多次执行取平均值。WebSocket请求延迟是建立连接，多次发送消息计算平均值。具体脚本参考test/benchmark文件夹

**测试结果**:
| 指标 | 平均值(ms) |
|----------|--------|
| REST请求 | 62 |
| WebSocket请求 | 426 |


### b.行情延迟测试
**测试说明**: 订阅BTC-USDT行情，持续运行24小时，统计交易所推送的间隔时间

**测试结果**:
| 指标 | 最小值(ms) | 最大值(ms) | 平均值(ms) | P50(ms) | P75(ms) | P90(ms) |P95(ms) | P99(ms) | 重连次数 |
|----------|--------|--------|--------|-----|-----|-----|-----|-----|-----|
| BookTicker | 0 | 24767 | 178.2 | 102 | 228 | 399 | 484 | 499 | 0 |

### c.交易延迟测试
**测试说明**: 发送不会成交的限价订单

**测试结果**:
| 指标 | 最小值(ms) | 最大值(ms) | 平均值(ms) |
|----------|--------|--------|--------|
| REST下单响应 | 1094 | 1955 | 1521.4 | 
| REST下单时，订单状态推送延迟 | 1105 | 1893 | 1389.3 | 

**测试结果**: REST下单各阶段延迟
| 指标 | 最小值(ms) | 最大值(ms) | 平均值(ms)
|----------|--------|--------|--------|
| 系统发送到交易所创建订单 | 1097 | 1877 | 1379 | 
| 交易所创建订单到更新状态 | 0 | 6 | 2.3 | 
| 交易所更新状态到系统接收 | 5 | 12 | 8 | 

## API接口


### 接口调用特殊说明

```cpp
// 使用时publicKey 对应 INFRA_API_KEY
//      privateKey 对应 INFRA_API_SECRET
//      publicKeyYCoordinate 对应 INFRA_API_PHRASE

// 市价单买入时需要请求接口获取24h quote中的oraclePrice字段，因此延迟会比较高
void place_order_rest(const SpOrder& order, OrderCallback cb)；

// 必须传入有效symbol，不支持传空查所有
void get_position(const Symbol& symbol, PositionCallback cb)；
UMSymbolPosition get_position(const Symbol& symbol)；
```

### 不支持的接口

```cpp
// 不支持设置持仓模式
bool set_position_mode(PositionMode mode);

// 不支持设置保证金模式
bool set_margin_mode(const Symbol& symbol, MarginMode mode);
// 不支持设置杠杆
bool set_leverage(const Symbol& symbol, unsigned int leverage, MarginMode mode)
// 独立成交数据订阅（订单推送已包含成交信息）
bool subscribe_trade(OrderCallback cb);
void unsubscribe_trade();

// 不支持ws报撤单接口
void place_order_ws(const SpOrder& order, OrderCallback cb)
void cancel_order_ws(const SpOrder& order, OrderCallback cb)
```
