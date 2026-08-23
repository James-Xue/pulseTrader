# Gate k线 REST API 铁律(实测,2026-08-23 验证)

> 本文档是 `project-memory.md`「fetch_klines / 回测数据」一节的细节展开。
> 所有结论均来自本机实际调用 Gate 公开 REST 验证,非文档推断。

## 三端点速查

| 端点 | 路径 | limit 上限 | 分页 | 响应格式 | 数据新鲜度(实测) |
|---|---|---|---|---|---|
| 合约 | `/api/v4/futures/usdt/candlesticks` | **2000** | `from/to` 可用但**与 limit 互斥** | **对象数组** `{t,o,h,l,c,v}` | ~33s(含当前未收盘根) |
| 现货 | `/api/v4/spot/candlesticks` | **1000** | 超窗报 `INVALID_PARAM_VALUE "Candlestick range too broad"` | **数组** `[ts,成交额,close,high,low,open,成交量,closed]` | ~35s |
| 黄金 | `/api/v4/tradfi/symbols/XAUUSD/klines` | **500**(≈8.3h 的 1m) | **`from/to` 被忽略** | 包在 `data.list`,字段 `{o,c,h,l,t}`,**无成交量** | 周末休市,最新一根滞后 ~33h(周五收盘) |

## ⚠️ 关键坑(2026-08-23 新发现)

1. **黄金接口必须显式传 `kline_type=1m`** — 不传直接报
   `{"label":"INVALID_ARGUMENT","message":"Invalid parameter"}`。
   带 `kline_type=1m` 才有数据。项目记忆此前未记录此条。
2. **合约/现货 `limit` 与 `from/to` 互斥**(HTTP 400)→ 取最近窗口只传 limit,取历史窗口必须省略 limit。
3. **futures 解析器不设 `closed` 标志**(默认 false)→ 回测/预载数据需自行强制 `closed=true`,否则 strategyLoop 的 closed 门永不通过(M30 WarmupSeeder 已处理)。
4. 深历史(黄金 >8.3h)只能换 `kline_type=5m/15m/1d` 或持续收集。

## 数据质量对比(2026-08-23 交叉验证)

- sqlite `kline_bars`(引擎 WS 自录)vs REST 直拉 CSV(2881 根重叠):
  - 价格:2533 根完全一致,中位数偏差 0.0000% — 整体可信
  - 但 08-21 有 3 根坏蜡烛(close 76381.8 / 1613.43 / 2371.78,即北京 15:58~16:00)
  - 347 根(12%)成交量失真(只有真实值 0.1%~1%),特定时段(断连/重连)
- **REST 直拉永远干净** → M30 起 futures kline 停录,启动免热机改用 REST 预载

## 相关工具

- `tools/fetch_klines.py`(公开 REST 无需 key)→ `data/klines/*_1m.csv`(ts,open,high,low,close,volume);`python3 tools/fetch_klines.py [hours]`(默认 48h);`data/` 在 .gitignore,提交需 `git add -f`
- M30 回测/预载:SqliteKlineReader(SQLite-first)+ GateKlineFetcher(API 补缺)+ KlineLoader(合并去重+sanitizeCandles)
