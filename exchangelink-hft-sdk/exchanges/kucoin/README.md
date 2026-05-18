# KuCoin Exchange

当前支持统一账户

## 测试报告

### 服务器
- **地理位置**: 东京
- **AWS区域ID**: ap-northeast-1, Subnet: apne1-az4 (ap-northeast-1a).

### 测试机器
- **配置信息**: AWS，EC2，亚太地区-东京(ap-northeast-1)


### a.网络延迟测试
**测试说明**: REST请求延迟是使用curl命令请求公共接口，多次执行取平均值。WebSocket请求延迟是建立连接，多次发送消息计算平均值。具体脚本参考test/benchmark文件夹

**测试结果**:
| 指标 | 平均值(ms) |
|----------|--------|
| REST请求 | 26 |
| WebSocket请求 | 2.5 |


### b.行情延迟测试
**测试说明**: 订阅BTC-USDT行情，持续运行24小时，统计交易所推送的间隔时间

**测试结果**:
| 指标 | 最小值(ms) | 最大值(ms) | 平均值(ms) | P50(ms) | P75(ms) | P90(ms) |P95(ms) | P99(ms) | 重连次数 |
|----------|--------|--------|--------|-----|-----|-----|-----|-----|-----|
| BookTicker | 0 | 6954 | 13.2 | 1 | 3 | 13 | 44 | 269 | 1 |


### c.交易延迟测试
**测试说明**: 发送不会成交的限价订单

**测试结果**:
| 指标 | 最小值(ms) | 最大值(ms) | 平均值(ms) |
|----------|--------|--------|--------|
| REST下单响应 | 32 | 54 | 42.5 |
| REST下单时，订单状态推送延迟 | 33 | 19 | 41.6 |


## API接口


### 接口调用特殊说明
```cpp
// 订阅订单簿行情，支持深度: 1, 5, 50
bool subscribe_orderbook(const Symbols& symbols, unsigned int depth, OrderbookCallback cb);

// 订阅订单状态推送（包含成交信息）
bool subscribe_order(OrderCallback cb);
```

### 不支持的接口
```cpp
// 订阅成交状态推送（订单状态推送已包含成交信息）
bool subscribe_trade(OrderCallback cb);
void unsubscribe_trade();
```
