# M29 回测引擎(单策略 MVP,5 提交,954 绿,2026-08-23)

> `project-memory.md`「M29 回测引擎」一节的细节展开。

## 提交链(均已推送)

dcadc60 交易所 K 线端点+93xx 错误码 / 5c4744e 数据源层 / e2fcdca 回放+虚拟账户 / 3b14c44 报告 / bb1a480 子命令+数据净化

## 用法(常用,主记忆保留速记)

```bash
./run.sh backtest --strategy ema_resonance_scalper --symbol ETH_USDT \
  --from 2026-08-21 --to 2026-08-23 --quantity 20 --quanto 0.01 [--json x.json]
```

- 窗口缺省自动解析(coverage 或 7 天);quanto 内置表 ETH 0.01 / BTC 0.0001
- 错误路径非零退出:缺参 2 / 未知策略 1 / 坏时间 2

## src/backtest/ 库组件

- **IKlineSource 抽象**(`KlineSource.hpp`):`fetch(symbol, market_type, from_ms, to_ms)` / `writeBack`(默认 no-op);`findKlineGaps` / `sanitizeCandles` 纯函数
- **SqliteKlineReader**(仅 PULSE_ENABLE_SQLITE):本地优先,PK (symbol, open_time),按 market_type 过滤
- **GateKlineFetcher**:REST 分页拉取,双格式解析(spot 数组 / futures 对象)
- **KlineLoader**:SQLite-first + 缺口 API 补 + merge/dedup + 可选回写;sqlite 可 nullptr(降级纯 API)
- **ReplayDriver**:FeedHarness 无 I/O 驱动真实策略,同一 onKline 模板路径,cooldown 播种 0
- **BacktestAccount**:close 价即时成交,Flip/Independent 模式,PnL 公式同 PositionManager,quanto 缩放,费率三态(<0 免 / 0 默认 / >0 显式)
- **BacktestReport**:表格+JSON 导出
- **BacktestEngine**:trading.toml 实例播种 + 注册名校验

## 实测经验

1. Gate limit 与 from/to **互斥**(HTTP 400)→ 有条件省略
2. futures candlesticks 是**对象数组** `{t,o,h,l,c,v}`,spot 是数组 `[ts,quote_vol,c,h,l,o,base_vol,closed]`
3. **脏数据净化**:kline_bars 有 ETH 坏蜡烛(close 76403.9/1613.43 邻 2371,08-21 记录缺陷)→ sanitizeCandles 剔非正 OHLC/高低不一致/>25% 跳变,报告带警告
4. EmaResonance **迁移触发**:单调趋势永不发信号,回测序列需构造状态迁移;warmup=首信号前蜡烛数

## 端到端基准

ETH 08-21~23:2879 根(sqlite 优先+API 补齐+缓存回写)、72 信号 16 笔净 -5.72、胜率 25%(验证回归用)

## 后续待办

多策略聚合、控制面 MCP backtest 方法、orderbook_scalper、参数优化、intra-bar SL/TP、资金费率

## M30 关联

- 引擎启动免热机复用 KlineLoader(SQLite-first + API 补缺 + sanitize)→ 见 `WarmupSeeder` 与 `gate-kline-api.md`
- 回测 CLI 的 cache_writeback 仍会写回 futures kline_bars(显式工具,保留)
