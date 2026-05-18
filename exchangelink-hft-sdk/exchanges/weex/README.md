# Weex Exchange


## 测试报告

### 服务器
- **地理位置**: 日本-大阪
- **AWS区域ID**: AWS

### 测试机器
- **配置信息**: AWS，EC2，亚太地区-东京(ap-northeast-1)

### a.网络延迟测试
**测试说明**: REST请求延迟是使用curl命令请求公共接口，多次执行取平均值。WebSocket请求延迟是建立连接，多次发送消息计算平均值。具体脚本参考test/benchmark文件夹

**测试结果**:
| 指标 | 平均值(ms) |
|----------|--------|
| REST请求 | 29 |
| WebSocket请求 | 8 |


### b.行情延迟测试
**测试说明**: 订阅BTC-USDT行情，持续运行24小时，统计交易所推送的间隔时间

**测试结果**:
| 指标 | 最小值(ms) | 最大值(ms) | 平均值(ms) | P50(ms) | P75(ms) | P90(ms) | P95(ms) | P99(ms) | 重连次数 |
|----------|--------|--------|--------|-----|-----|-----|-----|-----|-----|
| depth.15 | 441 | 558 | 500.0 | 499 | 499 | 499 | 499 | 499 | 499 |


### c.交易延迟测试
**测试说明**: 发送不会成交的限价订单

**测试结果**:
| 指标 | 最小值(ms) | 最大值(ms) | 平均值(ms) |
|----------|--------|--------|--------|
| REST下单响应 | 24 | 48 | 36.5 |
| REST下单时，订单状态推送延迟 | 24 | 47 | 38.2 |


## API接口

### 接口调用特殊说明

### 不支持的接口

TODO: 补充不支持的接口列表，例如：
```cpp
// 不支持设置持仓模式（仅支持双向持仓模式）
bool set_position_mode(PositionMode mode);

// 示例：不支持 WebSocket 下单
void place_order_ws(const SpOrder order, OrderCallback cb); // 内部转发到 REST
void cancel_order_ws(const SpOrder order, OrderCallback cb); // 内部转发到 REST
```
