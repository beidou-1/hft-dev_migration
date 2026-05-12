# Bybit Exchange

当前支持统一账户2.0

## 测试报告

### 服务器
- **地理位置**: 新加坡
- **AWS区域ID**: AWS Singapore, Availability Zone ID apse1-az2 & az3.

### 测试机器
- **配置信息**: AWS，EC2，亚太地区-新加坡(ap-southeast-1)


### a.网络延迟测试
**测试说明**: REST请求延迟是使用curl命令请求公共接口，多次执行取平均值。WebSocket请求延迟是建立连接，多次发送消息计算平均值。具体脚本参考test/benchmark文件夹

**测试结果**:
| 指标 | 平均值(ms) |
|----------|--------|
| REST请求 | 32 |
| WebSocket请求 | 2.5 |


### b.行情延迟测试
**测试说明**: 订阅BTC-USDT行情，持续运行24小时，统计交易所推送的间隔时间

**测试结果**:
| 指标 | 最小值(ms) | 最大值(ms) | 平均值(ms) | P50(ms) | P75(ms) | P90(ms) |P95(ms) | P99(ms) | 重连次数 |
|----------|--------|--------|--------|-----|-----|-----|-----|-----|-----|
| BookTicker | 0 | 3500 | 75.2 | 20 | 61 | 190 | 330 | 499 | 1 |


### c.交易延迟测试
**测试说明**: 发送不会成交的限价订单

**测试结果**:
| 指标 | 最小值(ms) | 最大值(ms) | 平均值(ms) |
|----------|--------|--------|--------|
| REST下单响应 | 10 | 40 | 16.1 | 
| REST下单时，订单状态推送延迟 | 12 | 19 | 14.4 | 
| WebSocket下单响应 | 3 | 22 | 7.3 |
| WSS下单时，订单状态推送延迟 | 5 | 15 | 7.7 | 


**测试结果**: REST下单各阶段延迟
| 指标 | 最小值(ms) | 最大值(ms) | 平均值(ms)
|----------|--------|--------|--------|
| 系统发送到交易所创建订单 | 9 | 15 | 10.8 | 
| 交易所创建订单到更新状态 | 1 | 2 | 1.3 | 
| 交易所更新状态到系统接收 | 2 | 3 | 2.3 | 


## API接口

### 接口调用特殊说明
```cpp
// 订阅订单簿行情，支持深度: 1
bool subscribe_orderbook(const Symbols& symbols, unsigned int depth, OrderbookCallback cb);

### 不支持的接口
```cpp
// 统一账户暂不支持API设置保证金模式
bool set_margin_mode(const Symbol& symbol, MarginMode mode);

// 独立成交数据订阅（订单推送已包含成交信息）
bool subscribe_trade(OrderCallback cb);
void unsubscribe_trade();
```