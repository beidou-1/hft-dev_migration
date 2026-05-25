# OKEX Exchange

当前支持统一账户的跨币种保证金模式

## 测试报告

### 服务器
- **地理位置**: 香港
- **区域ID**: 阿里云d区，备用节点在b和c区

### 测试机器
- **配置信息**: AWS，EC2，亚太地区-香港


### a.网络延迟测试
**测试说明**: REST请求延迟是使用curl命令请求公共接口，多次执行取平均值。WebSocket请求延迟是建立连接，多次发送消息计算平均值。具体脚本参考test/benchmark文件夹

**测试结果**:
| 指标 | 平均值(ms) |
|----------|--------|
| REST请求 | 36 |
| WebSocket请求 | 4.3 |


### b.行情延迟测试
**测试说明**: 订阅BTC-USDT行情，持续运行24小时，统计交易所推送的间隔时间

**测试结果**:
| 指标 | 最小值(ms) | 最大值(ms) | 平均值(ms) | P50(ms) | P75(ms) | P90(ms) |P95(ms) | P99(ms) | 重连次数 |
|----------|--------|--------|--------|-----|-----|-----|-----|-----|-----|
| BookTicker | 10 | 2750 | 46.5 | 20 | 50 | 110 | 180 | 360 | 0 |
| 5档行情 | 10 | 2900 | 128.6 | 100 | 100 | 200 | 300 | 499 | 0 |


### c.交易延迟测试
**测试说明**: 发送不会成交的限价订单

**测试结果**:
| 指标 | 最小值(ms) | 最大值(ms) | 平均值(ms) |
|----------|--------|--------|--------|
| REST下单响应 | 59 | 69 | 64.8 | 
| REST下单时，订单状态推送延迟 | 61 | 66 | 63.9 | 
| WebSocket下单响应 | 6 | 14 | 9.7 |
| WSS下单时，订单状态推送延迟 | 6 | 13 | 9.7 | 


**测试结果**: REST下单各阶段延迟
| 指标 | 最小值(ms) | 最大值(ms) | 平均值(ms)
|----------|--------|--------|--------|
| 系统发送到交易所创建订单 | 58 | 64 | 61.1 | 
| 交易所创建订单到更新状态 | 0 | 0 | 0 | 
| 交易所更新状态到系统接收 | 2 | 3 | 2.8 | 


### d.Benchmark参考

**说明**: 由外部人员提供，来自做市商白名单的测试数据

| 指标 | 均值(ms) |
|----------|--------|
| WebSocket下单 | 1.1~1.24 |


## API接口

### 接口调用特殊说明
```cpp
// 订阅订单簿行情，支持深度: 1, 5
bool subscribe_orderbook(const Symbols& symbols, unsigned int depth, OrderbookCallback cb);

// 订阅订单状态推送（包含成交信息）
bool subscribe_order(OrderCallback cb);

// 设置杠杆倍数（同时应用于多空方向）
bool set_leverage(const Symbol& symbol, unsigned int leverage, MarginMode mode);

// 设置持仓模式（支持单向/双向持仓模式）
bool set_position_mode(PositionMode mode);
```

### 不支持的接口
```cpp
// OKX使用跨币种保证金模式，只支持全仓
bool set_margin_mode(const Symbol& symbol, MarginMode mode);

// 独立成交数据订阅（订单推送已包含成交信息）
bool subscribe_trade(OrderCallback cb);
void unsubscribe_trade();
```
