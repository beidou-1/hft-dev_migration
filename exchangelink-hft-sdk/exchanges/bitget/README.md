# Bitget Exchange

当前支持统一账户

## 测试报告

### 服务器
- **地理位置**: 东京
- **AWS区域ID**: AWS Tokyo ap-northeast-1a (apne1-az4), AWS Tokyo ap-northeast-1c (apne1-az1)

### 测试机器
- **配置信息**: AWS，EC2，亚太地区-东京(ap-northeast-1)


### a.网络延迟测试
**测试说明**:: REST请求延迟是使用curl命令请求公共接口，多次执行取平均值。WebSocket请求延迟是建立连接，多次发送消息计算平均值。具体脚本参考test/benchmark文件夹

**测试结果**:
| 指标 | 平均值(ms) |
|----------|--------|
| REST请求 | 54 |
| WebSocket请求 | 5 |


### b.行情延迟测试
**测试说明**: 订阅BTC-USDT行情，持续运行24小时，统计交易所推送的间隔时间

**测试结果**:
| 指标 | 最小值(ms) | 最大值(ms) | 平均值(ms) | P50(ms) | P75(ms) | P90(ms) |P95(ms) | P99(ms) |
|----------|--------|--------|--------|-----|-----|-----|-----|-----|
| BookTicker | 19 | 4830 | 60.6 | 20 | 60 | 140 | 221 | 499 |
| 5档行情 | 99 | 3200 | 155.2 | 100 | 200 | 300 | 400 | 499 |


### c.交易延迟测试
**测试说明**: 发送不会成交的限价订单

**测试结果**:
| 指标 | 最小值(ms) | 最大值(ms) | 平均值(ms) |
|----------|--------|--------|--------|
| REST下单响应 | 75 | 100 | 86.2 | 
| REST下单时，订单状态推送延迟 | 75 | 104 | 86.8 | 
| WebSocket下单响应 | 8 | 17 | 12.7 |
| WSS下单时，订单状态推送延迟 | 8 | 23 | 15.3 | 


**测试结果**: REST下单各阶段延迟
| 指标 | 最小值(ms) | 最大值(ms) | 平均值(ms)
|----------|--------|--------|--------|
| 系统发送到交易所创建订单 | 69 | 96 | 81.3 | 
| 交易所创建订单到更新状态 | 0 | 0 | 0 | 
| 交易所更新状态到系统接收 | 4 | 8 | 5.6 | 


## API接口

### 接口调用特殊说明
```cpp
// 订阅订单簿行情，支持深度: 1, 5, 50
bool subscribe_orderbook(const Symbols& symbols, unsigned int depth, OrderbookCallback cb);

// 设置杠杆倍数（同时应用于多空方向）
bool set_leverage(const Symbol& symbol, unsigned int leverage, MarginMode mode);

// 调用该接口可以订阅订单状态推送信息
bool subscribe_order(OrderCallback cb);
```

### 不支持的接口
```cpp
// Bitget统一账户暂不支持API设置保证金逐仓全仓
bool set_margin_mode(const Symbol& symbol, MarginMode mode);

// subscribe_order订阅的信息推送已包含成交
bool subscribe_trade(OrderCallback cb);
void unsubscribe_trade();
```