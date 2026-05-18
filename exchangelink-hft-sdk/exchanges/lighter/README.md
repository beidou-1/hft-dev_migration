# Lighter Exchange

当前支持USDC永续合约

## 测试报告

### 服务器
- **地理位置**: 未知

**典型地区测试结果**:
| 东京 | 新加坡 | 香港 | 弗吉尼亚 | 爱尔兰 |
|------|-------|------|---------|-------|
| 29 | 68 | 82 | 85 | 292 |


### 测试机器
- **配置信息**: AWS，EC2，亚太地区-东京(ap-northeast-1)


### a.网络延迟测试
**测试说明**: REST请求延迟是使用curl命令请求公共接口，多次执行取平均值。WebSocket请求延迟是建立连接，多次发送消息计算平均值。具体脚本参考test/benchmark文件夹

**测试结果**:
| 指标 | 平均值(ms) |
|----------|--------|
| REST请求 | 29 |
| WebSocket请求 | 5.5 |


### b.行情延迟测试
**测试说明**: 订阅BTC-USDC行情，持续运行24小时，统计交易所推送的间隔时间

**测试结果**:
| 指标 | 最小值(ms) | 最大值(ms) | 平均值(ms) | P50(ms) | P75(ms) | P90(ms) |P95(ms) | P99(ms) | 重连次数 |
|----------|--------|--------|--------|-----|-----|-----|-----|-----|-----|
| BookTicker | 5 | 1599 | 65.5 | 50 | 53 | 101 | 151 | 299 | 1 |


### c.交易延迟测试
**测试说明**: 发送不会成交的限价订单

**测试结果**:
| 指标 | 最小值(ms) | 最大值(ms) | 平均值(ms) |
|----------|--------|--------|--------|
| REST下单响应 | 19 | 25 | 22.1 | 
| REST下单时，订单状态推送延迟 | 323 | 332 | 327.6 | 
| WebSocket下单响应 | 11 | 26 | 12.7 |
| WSS下单时，订单状态推送延迟 | 325 | 362 | 334.6 | 


## API接口
```cpp
AccountSecret secret;
secret.api_secret = "aaa";
secret.custom_info["api_key_index"] = "3";
secret.custom_info["api_token"] = "bbb";
secret.custom_info["account_id"] = "123";
```
说明：在网页端创建key，获取secret_id和api_secret, 再创建token，从token中获取account_id。

### 接口调用特殊说明
```cpp
// 订阅订单簿行情，支持深度: 5档以内均支持
bool subscribe_orderbook(const Symbols& symbols, unsigned int depth, OrderbookCallback cb);
```

### 不支持的接口
```cpp
bool set_margin_mode(const Symbol& symbol, MarginMode mode);
bool set_position_mode(PositionMode mode);
```