# WebUI K线图不显示 — 根因分析与修复方案

> **日期**: 2026-06-20
> **状态**: 分析完成，待修复
> **前端现象**: K-line Chart 面板显示 "Waiting for candle data..."

## 1. 数据管道全链路追踪

```
Gate.io Futures WS (fx-ws.gateio.ws)
  ↓ [futures.candlesticks 频道订阅]
MarketFeed::on_kline_update()          ← 解析 OHLCV JSON → push 到 KlineBuffer
  ↓
KlineBuffer (per-symbol, 500 根环形缓冲, seqlock)
  ↓
DashboardState::poll_klines()          ← 每 200ms 检测新 K 线
  ↓
WsServer::push_snapshot()              ← JSON 序列化 → uWebSockets 广播
  ↓
前端 renderKline(snap.kline)           ← 渲染 OHLCV 表格（非图形蜡烛图）
```

**关键代码路径**:
- `src/market/market_feed.cpp:278-344` — `on_kline_update()` 解析 Gate.io JSON
- `src/market/kline_buffer.hpp` — 环形缓冲区 (500 candles, seqlock)
- `src/webui/dashboard_state.cpp:229-262` — `poll_klines()` 新 K 线检测
- `src/webui/ws_server.cpp:57-77` — WebSocket 广播
- `frontend/app.js:235-266` — `renderKline()` 渲染为 HTML 表格

## 2. 日志证据

### 2.1 WS 连接和订阅（正常）

```
[exchange.log 17:03:19-21]
WS proxy tunnel active: ... → wss://fx-ws.gateio.ws/v4/ws/usdt
WS connected to wss://api.gateio.ws/ws/v4/
WS subscribed to futures.tickers
WS subscribed to futures.order_book
WS subscribed to futures.candlesticks
```

WS 成功连接并保持 13 分钟不断开（17:03 → 17:15），网络层正常。

### 2.2 行情数据（零事件）

```
[market.log 17:03:19-21]
Starting futures MarketFeed for 1 symbols
Loaded 62 futures instruments from REST
futures MarketFeed started — subscribed to 1 symbols
[... 13 分钟内没有任何 ticker/kline 事件日志 ...]
[market.log 17:15:59]
Stopping MarketFeed
```

### 2.3 策略诊断（完全静默）

```
[strategy.log 17:03:21]
Started strategy: MomentumScalper on BTC_USDT
[MomentumScalper] Thread started, polling every 500ms
[... 13 分钟内没有 "Waiting for kline data" 或 "Warming up" 日志 ...]
[strategy.log 17:15:59]
[MomentumScalper] Thread exiting
```

**结论**: Gate.io 服务器接受了 WS 连接和订阅，但在 13 分钟内未推送任何行情数据帧。

## 3. 发现的 Bug

### Bug #1（关键）：`poll_klines()` 依赖 ticker_cache 作为 symbol 索引

**文件**: `src/webui/dashboard_state.cpp:229-262`

```cpp
void DashboardState::poll_klines(DashboardSnapshot &snap)
{
    // ❌ 用 ticker_cache 的 symbol 列表来决定查哪个 kline buffer
    const auto symbols = market_feed_.ticker_cache().symbols();
    if (symbols.empty())
    {
        return;  // ← ticker 没数据就完全放弃 kline，即使 kline buffer 有数据
    }

    const auto &symbol = symbols[0];
    auto &kline_buf = market_feed_.get_kline_buffer(symbol);
    const auto latest_kline = kline_buf.latest();
    if (!latest_kline.has_value()) return;
    // ...
}
```

**问题**: K 线和 Ticker 是独立的 WS 频道。`poll_klines()` 不应该依赖 ticker 数据来决定是否检查 kline。即使 kline 数据已到达，ticker 缓存为空会导致 kline 数据被完全忽略。

**修复方案**: 改为遍历 `subscribed_symbols_` 或 `kline_buffers_` 的 key，不依赖 `ticker_cache().symbols()`。

```cpp
// 修复建议：使用 subscribed_symbols 替代 ticker_cache().symbols()
void DashboardState::poll_klines(DashboardSnapshot &snap)
{
    // 改用 MarketFeed 提供的 subscribed symbols 列表
    const auto symbols = market_feed_.subscribed_symbols();  // 需要新增此方法
    // 或者直接遍历 kline_buffers_ 的所有 key
    for (const auto &symbol : symbols)
    {
        auto &kline_buf = market_feed_.get_kline_buffer(symbol);
        auto latest = kline_buf.latest();
        if (!latest.has_value()) continue;
        // ... 检测新 K 线并更新 snap
    }
}
```

### Bug #2（中等）：策略诊断日志依赖 ticker 到达

**文件**: `src/strategy/strategy_manager.cpp:196-201`

```cpp
// 1. Poll ticker for on_tick().
auto ticker_opt = feed->ticker_cache().get(cfg.symbol);
if (ticker_opt.has_value())      // ← ticker 没数据就跳过
{
    strategy.on_tick(ticker_opt.value());
}
```

**问题**: `on_tick()` 包含 "Waiting for kline data" 诊断逻辑，但只在 ticker 数据到达后才被调用。ticker 不来 → `on_tick()` 永远不执行 → 策略诊断完全静默 → 用户无法判断程序状态。

**修复方案**: ticker 为空时也触发诊断（传入空 ticker 或新增 `on_no_data()` 回调）。

```cpp
// 修复建议：ticker 为空时也通知策略
auto ticker_opt = feed->ticker_cache().get(cfg.symbol);
if (ticker_opt.has_value())
{
    strategy.on_tick(ticker_opt.value());
}
else
{
    // 传入默认空 ticker，让 on_tick() 的诊断分支执行
    strategy.on_tick(market::Ticker{});
}
```

### Bug #3（辅助）：WS 数据帧无 INFO 级别日志

**文件**: `src/market/market_feed.cpp` — `on_ticker_update()`, `on_kline_update()`

**问题**: 这两个回调在成功接收并处理数据时**不产生 INFO 级别日志**。导致无法从运行时日志判断 Gate.io 是否在推送数据。

**修复方案**: 添加首次数据接收的 INFO 日志（每个 symbol 只记录一次）。

```cpp
// 修复建议：在 on_kline_update() 中添加首次数据日志
void MarketFeed::on_kline_update(const nlohmann::json &result, const nlohmann::json &full_frame)
{
    // ... 解析 kline ...

    auto &buffer = get_kline_buffer(symbol);
    if (buffer.size() == 0)  // 第一根 K 线
    {
        PULSE_LOG_INFO("market", "First kline received for {} (open_time={}, close={:.2f})",
                       symbol, kline.open_time, kline.close);
    }
    buffer.push(kline);
}
```

### Bug #4（辅助）：WS 连接日志使用了错误的 URL

**文件**: `src/exchange/gate_ws_client.cpp:842`

```cpp
PULSE_LOG_INFO("exchange", "WS connected to {}", config_.wsUrl);  // ← 始终打印 spot URL
```

**问题**: 无论实际连接的是 spot 还是 futures WS 服务器，日志始终打印 `config_.wsUrl`（spot URL）。导致排查时产生误导。

**修复方案**: 打印实际使用的 `ws_url` 变量。

```cpp
PULSE_LOG_INFO("exchange", "WS connected to {}", ws_url);
```

## 4. 待确认的外部问题

即使修复上述代码 Bug，仍需确认 Gate.io Futures WS 是否正常推送数据：

| 检查项 | 方法 |
|--------|------|
| Gate.io Futures WS 是否真的不发数据 | 修复 Bug #3 后重新运行，检查 market.log |
| 代理是否干扰 futures WS | 临时关闭代理直连测试（如果网络允许） |
| Gate.io API 变更 | 用 `wscat` 手动连接 `wss://fx-ws.gateio.ws/v4/ws/usdt` 发送订阅消息验证 |
| futures 合约名 "BTC_USDT" 是否正确 | REST `/api/v4/futures/usdt/contracts` 返回的 name 字段 |

### 手动验证命令

```bash
# 用 wscat 测试 Gate.io futures WS（需要代理）
wscat -c "wss://fx-ws.gateio.ws/v4/ws/usdt" -x '{"time":1718900000,"channel":"futures.candlesticks","event":"subscribe","payload":["BTC_USDT","1m"]}'

# 或用 curl 测试 REST 接口确认合约名
curl -s "https://api.gateio.ws/api/v4/futures/usdt/contracts" | jq '.[0].name'
```

## 5. 额外发现：前端 K 线面板是表格而非图形

当前 `frontend/app.js:235-266` 的 `renderKline()` 将 K 线数据渲染为 **HTML 表格**（Time/O/H/L/C/V + 红绿箭头），不是传统的蜡烛图。

**后续改进**（非阻塞）：引入 [TradingView Lightweight Charts](https://github.com/nicehash/lightweight-charts)（~40KB gzipped），实现真正的蜡烛图渲染。

## 6. 修复优先级

| 优先级 | Bug | 影响 |
|--------|-----|------|
| 🔴 P0 | #1 poll_klines() 依赖 ticker_cache | K 线面板永远无法显示（直接根因） |
| 🟡 P1 | #2 策略诊断依赖 ticker | 用户无法判断程序是否在正常工作 |
| 🟡 P1 | #3 WS 数据帧无日志 | 无法从日志判断 WS 是否在收数据 |
| 🟢 P2 | #4 WS 连接日志 URL 错误 | 排查误导 |
