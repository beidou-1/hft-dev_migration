# Phemex Exchange

交易所测试报告和接口说明

## 测试报告

### 服务器
- **地理位置**: 未知

**典型地区测试结果**:
| 东京 | 新加坡 | 香港 | 弗吉尼亚 | 爱尔兰 |
|------|-------|------|---------|-------|
| 95 | 28 | 61 | 239 | 219 |


### 测试机器
- **配置信息**: AWS，EC2，亚太地区-新加坡(ap-southeast-1)


### a.网络延迟测试
**测试说明**: REST请求延迟是使用curl命令请求公共接口，多次执行取平均值。WebSocket请求延迟是建立连接，多次发送消息计算平均值。具体脚本参考test/benchmark文件夹

**测试结果**:
| 指标 | 平均值(ms) |
|----------|--------|
| REST请求 | 26 |
| WebSocket请求 | 16.4 |


### b.行情延迟测试
**测试说明**: 订阅BTC-USDT行情，持续运行24小时，统计交易所推送的间隔时间

**测试结果**:
| 指标 | 最小值(ms) | 最大值(ms) | 平均值(ms) | P50(ms) | P75(ms) | P90(ms) |P95(ms) | P99(ms) | 重连次数 |
|----------|--------|--------|--------|-----|-----|-----|-----|-----|-----|
| BookTicker | 0 | 15369 | 415.2 | 80 | 371 | 499 | 499 | 499 | 0 |

### c.交易延迟测试
**测试说明**: 发送不会成交的限价订单

**测试结果**:
| 指标 | 最小值(ms) | 最大值(ms) | 平均值(ms) |
|----------|--------|--------|--------|
| REST下单响应 | 11 | 15 | 13 | 
| REST下单时，订单状态推送延迟 | 12 | 98 | 23 | 

**测试结果**: REST下单各阶段延迟
| 指标 | 最小值(ms) | 最大值(ms) | 平均值(ms)
|----------|--------|--------|--------|
| 系统发送到交易所创建订单 | 5 | 11 | 7.7 | 
| 交易所创建订单到更新状态 | 2 | 6 | 3.9 | 
| 交易所更新状态到系统接收 | 2 | 87 | 11.5 | 

## API接口


### 接口调用特殊说明
```cpp
// 订阅订单簿行情，支持深度: 1, 5, 10, 30
// (单个client同时最多建立5个ws连接，除去subscribe order外，订单簿最多可以建立4个连接，每个连接最大订阅数为20，代码中设置为19，因此订阅的最大行情数量为4*19=76个)
bool subscribe_orderbook(const Symbols& symbols, unsigned int depth, OrderbookCallback cb);

// 订阅订单状态推送（包含成交信息）
bool subscribe_order(OrderCallback cb);

// 设置杠杆倍数（同时应用于多空方向）
bool set_leverage(const Symbol& symbol, unsigned int leverage, MarginMode mode);

// currency为必填字段
void get_balance(const Currency& currency, BalanceCallback cb);
```

### 不支持的接口
```cpp
// 设置持仓模式（需要传入symbol，暂不兼容，默认使用hedge模式, 如需改成one_way模式, 需要修改phemex_utils.h中的g_current_position_mode后重新编译）
bool set_position_mode(PositionMode mode);

// 
bool set_margin_mode(const Symbol& symbol, MarginMode mode);
```