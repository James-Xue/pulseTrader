# pulseTrader — Project Memory

> Last updated: 2026-08-17
> File size: 23655 chars / 25000 chars. Must recalculate and sync this line after updating this file.
> Historical details migrated to `project-memory-archive.md`

## Overview

- **Project**: pulseTrader — C++20 high-frequency scalping framework
- **Repository**: https://github.com/James-Xue/pulseTrader (public, GPL 3.0)
- **Exchange**: Gate.io (REST + WebSocket), single-exchange focus
- **Namespace**: `pulse::` · **Build**: CMake + vcpkg

## Architecture (9 Layers, All ✅)

| L1 Exchange | L2 Logging | L3 Market Data | L4 AI Analysis | L5 Heartbeat |
|---|---|---|---|---|
| L6 Strategy | L7 Risk Mgmt | L8 Execution | L9 Control Plane | — |

- Hot path (L1→L3→L6→L7→L8) vs AI background (L4→L5), bridged via `std::atomic`
- Control plane (L9, `headless` branch, replaces WebUI): JSON-RPC socket 127.0.0.1:8081 + embedded/remote REPL + stdio MCP server
- Proxy: REST via `CURLOPT_PROXY`; WS via `ProxyTunnel` class
- Credentials: `.env` (`GATE_API_KEY`/`GATE_API_SECRET`), gitignored

## Dependencies

- Core: nlohmann-json, spdlog, fmt, curl, openssl, asio, websocketpp, gtest, toml11
- Optional: sqlitecpp (`-DPULSE_ENABLE_SQLITE=ON`)
- Vendored: websocketpp in `third_party/` (uWebSockets/uSockets removed with the WebUI)
- SQLiteCpp GCC 15 fix: build with `-DCMAKE_CXX_FLAGS="-include cstdint"`

## Current State (M20 Done, 2026-08-17)

### Test Summary
- **745 tests all green** (CTest, `headless` branch): control plane (CommandParser, JsonRpcServer, McpServer, OrderFlowTest incl. M15 direction-gate + M16 maker-first + M17 per-market budget + M18 sink/recorder/migration tests + M20 signal-only tests + M21 sync/modify-sl-tp tests, EngineServicesTest incl. switch + signals + sync + modify tests, ControlClient) + core/config/logger/exchange/market/execution/risk/strategy/AI/heartbeat/trade_recorder suites
- M15 additions: gate rejects inactive market, switch allows cfd + rejects futures, reduce_only exemption, signal skip, cancel sweep, switchDirection (unconfigured fails / unknown rejected / noop / to-spot), openOrder defaults to active market, market() feed selection, evaluateCfdOrder (7101/7102), parseCfdDetail/validateOrder/mergeFrom, buildOrderBody CFD, cfd endpoint paths, REPL switch, parseCfdTicker/parseCfdKline
- M16 (2026-08-16) additions: maker-first order flow — 14 OrderFlowTest (best bid/ask post-only pricing, sweep cancel+fallback, partial-fill remainder, exchange-reject no-chase, rate-limit/direction-switch rejection, cancel-race) + 5 config validator + 3 config loader = 22 new
- M17 (2026-08-17) additions: per-market notional budget (PositionManager reserveNotional/openPosition/canOpenPosition filtered by market_type; optional maxPositionNotional{Futures,Cfd,Spot} with maxPositionNotional fallback; canOpenPosition quanto fix) + CFD order-id resolution (matchCfdOrderId — POST /tradfi/orders does not echo the order id) = 18 new
- M20 (2026-08-17) additions: signal-only mode + SignalBoard — 6 SignalBoardTest + 2 OrderFlowTest (signal-only skips execution / manual orders still work) + 1 config loader + 2 command parser (signals, open --sl/--tp) + MCP tools 17→18 + 3 EngineServicesTest (empty board / published entries / registry) = 28 new
- OrderFlowTest 2026-08-14 regressions: OnSignalModifiedOrderIsPlaced, OnSignalModifiedFailureReleasesReservation, FuturesQuantoKeepsFullContractQuantity, SellFillOpensShortWhenNoLong

### Milestones
- **M1–M5** ✅: Core pipeline → strategy → risk → AI → control plane → trading engine
- **M6** ✅: TOML config (`--config trading.toml`, `from_env:` syntax)
- **M7** ✅: SQLite trade recorder (17-col schema, 4 queries)
- **M8** ✅: Futures config foundation (MarketType/MarginMode enums, 7xxx errors)
- **M9** ✅: EndpointRouter + WS ping/pong fix
- **M10** ✅: Futures Market Data — Ticker mark/index/funding fields; SymbolInfo (quanto, leverage, maintenance, order_size, market_type); SymbolRegistry MarketType + parse_futures_contract; MarketFeed MarketType routing; EndpointRouter orders/leverage paths; 11 tests
- **M11** ✅: Futures Risk / PnL — Position (market_type, leverage, margin, liq, quanto); PositionManager leverage-aware PnL + futures openPosition; RiskManager evaluate_futures_order (7xxx codes); 12 tests
- **M12** ✅: Futures Execution + Dual-Market — OrderRequest market_type/leverage/reduce_only/contract_size; OrderExecutor/OrderTracker MarketType routing; TradingSignal market_type; main.cpp dual-market wiring; 7 tests
- **M13** ✅: Testnet — `PULSE_NETWORK` mainnet/testnet; testnet REST api-testnet.gateapi.io; testnet WS falls back to mainnet fx-ws (unreachable from China, data identical); config testnet=true overrides; .env GATE_MAINNET_*/GATE_TESTNET_* split; 6 tests

### Post-M13 Bugfixes (2026-06-20) → 详情见 project-memory-archive.md
Ctrl+C 3 层修复 · 预热诊断 · aggregator 阈值 0.7→0.6 · OPERATIONAL_GUIDE §4.4/§5.1/Q7

### Architecture Review Fixes (2026-06-20) → 详情见 project-memory-archive.md
PnL→DrawdownGuard · AI 反馈回路 · safe_parse_double 防 stod 崩溃（3 commit）

### Testnet 修复 (2026-06-21) → 详情见 project-memory-archive.md
URL 自动切换 4 测试（`pulse::url` 6 常量）；testnet WS 走主网 fx-ws（CloudFront TLS 不兼容）；笔记本环境 apt 依赖清单

### System Heartbeat Logging (2026-06-21)
`FeedStats` 原子计数器 + 60s heartbeat 行（uptime/各 feed 速率/WS 状态/策略/持仓/账户），热路径 1 次 relaxed fetch_add

### WebUI History (removed 2026-08-13 on `headless` branch)
Fully superseded by the control plane (JSON-RPC/REPL/MCP); all fixes are in git history.

### Account Balance (2026-06-21, still relevant)
- **AccountBalance struct**: total, available, unrealised_pnl, position_margin, order_margin, currency
- **REST parsing**: `GateRestClient::get_futures_account_balance()` — parses Gate.io futures account JSON (all values as strings → safe_parse_double)
- **Heartbeat log**: `... | account 1000.00 USDT (avail 950.00, pnl +2.50)`
- `Result<T>` is `std::variant<T, PulseError>` — use `ok()` / `value()` / `error()`, not `has_value()`

### 2026-06-23 重构与构建 → 详情见 project-memory-archive.md
命名重构（camelCase/m_ 前缀/文件名 PascalCase，547 绿）· Fast Ctrl+C `0c1a7ed` · vcpkg 兼容 `a812333`

### M17 (2026-08-17, done — 687 tests, committed 5504e3f/e38ed1b/1ced101)
- **Per-market notional budget**: cap enforced per market type — RiskConfig optional `maxPositionNotionalFutures/Cfd/Spot` (fallback maxPositionNotional); PositionManager reserveNotional/openPosition/canOpenPosition filter by market_type via `notionalLimitFor(mt)`; canOpenPosition quanto fix; maxOpenPositions/maxSymbolNotional stay global. Fixes: SKHY futures (5099) ate the CFD budget → 0.01-lot XAUUSD clamped to 0.003 → VOLUME_LESS_THAN_MIN_LIMIT.
- **CFD order-id fix**: POST /tradfi/orders echoes no order id (`data.id` = internal number) → `matchCfdOrderId` resolves from open-orders list (symbol/side/volume/price). Live-verified place→resolve→cancel.
- **SKHY closed**: SL 170.5 fired 11:33 (loss ≈ -263 USDT, 414→151). CFD: user's sell@4399 fired 12:02 → 0.01 short @4399.04 → later closed (35.95→37.18). Remaining user triggers: sell@4418 / buy@4295 / sell@4428 — KEEP. CFD acct 37.18 USD, futures 151.44 USDT. active_market=cfd; XAUUSD now runs 3 instances (momentum/mean_reversion/supertrend, added 12:55 local config only).

### M18 落库 (2026-08-17, done — 703 tests, 已提交 e9a34d8/05627bd)
- **任务**: 用户要求黄金实盘数据落库。方案文件: `~/.claude/plans/replicated-floating-key.md`。
- **Part A (trades 表 + 迁移)**: TradeRecorder DDL 追加 market_type/leverage/quanto 3 列 + `migrateSchema()`(user_version v1 + pragma_table_info 守卫防重复 ALTER) + open() busy_timeout=5000;recordTrade 3 默认参数(Spot/1.0/1.0);getTrades 位置读取 +3;TradeRecord +3 字段;OrderFlowExecutor onOrderComplete 传 reservation 的 market_type/leverage/quanto,strategy_name 改 matchInstanceConfig
- **Part B (行情落库)**: 新建 MarketDataSink.hpp(onTicker/onKline 虚接口,禁止阻塞契约);MarketFeed setMarketDataSink + 4 写入点(WS ticker/kline + CFD ticker/kline)+ Ticker.timestamp 改 nowMs();新建 MarketRecorder(POD 环队列 8192,溢出丢最旧,jthread 批量写 batch 128/1s,BEGIN IMMEDIATE,kline INSERT OR IGNORE PK(symbol,open_time),ticker_ticks/kline_bars 表,stop() drain+checkpoint 幂等,`Result<unique_ptr>`);SqliteConfig.recordMarketData + `record_market`(trading.toml=true,example=false);main.cpp 接线(失败仅 warn,关闭顺序 feeds→market_recorder→trade_recorder)
- **验证**: 703 测试绿;实盘库真实迁移(user_version=1、20 列、12 旧成交保留);实盘 ticker_ticks XAUUSD|cfd ~1/s + BTC_USDT|futures 13 行,kline_bars CFD backfill 500;引擎 13:22 重启生效(systemd)
- **提交建议**: Part A + Part B 一次提交(位置读取与 DDL 同 commit),备份 data/trades.db.bak-m18

### M20 Signal-Only + SignalBoard (2026-08-17, done — 731 tests, 已提交 0041a4a..d5daf08)
- **背景**:引擎 XAUUSD 策略(active_market=cfd 下自动运行)与 LLM 子代理双头交易风险。2026-08-17 17:06 事故实锤:旧引擎策略自动开 XAUUSD 多 0.01(entry 4397.7,exchange id 17657141),无任何 SL/TP,浮亏至 -7.5 后被用户手动平掉(CFD 账户 53.4 USD)。设计文档:`~/1_Code/commit_my_life/0_note/xauusd-signal-board-design.md`
- **signal_only 模式**:StrategyConfig `signal_only`(默认 false,TOML `[strategy] signal_only`);OrderFlowExecutor ctor 存 `m_signalOnly`,`onSignal` 入口短路(Flat 检查之后、方向门之前),手动 `placeOrder` 不受影响;main.cpp 聚合回调无论哪种模式都 publishAggregate(复盘可审计)。trading.toml 已设 `signal_only = true`(引擎策略从此只发信号)
- **SignalBoard**:新组件 `src/strategy/signal/SignalBoard.{hpp,cpp}` — 每 strategy_id 覆盖式保留最新 Entry(signal + ts_ms),聚合信号独立槽位(带 threshold),shared_mutex,`snapshot()` 输出 signals[] + aggregate;策略信号回调(main.cpp:919)双发 board+aggregator;TradingSignal 新增 `indicators`(json,默认 {})——四个策略已填:momentum(ema_fast/slow/diff)、mean_reversion(bb_upper/lower/mid)、supertrend(supertrend/dir/atr)、orderbook(imbalance/volumes/best bid-ask)
- **get_signals**:EngineServices::signals()(快照 + display-tz `ts_str`)+ JsonRpcServer `reg["get_signals"]` + MCP tool(18 个)+ REPL `signals` 命令(表格:SOURCE/SYMBOL/TYPE/CONF/PRICE/TIME + aggregate 行)
- **open_order 附加 SL/TP**:OrderRequest `sl_price`/`tp_price`(optional);buildOrderBody CFD 分支填 `price_sl`/`price_tp`(交易所原生保护,引擎挂了也止损——取代原设计的独立 place_trigger_order;备忘里的 price_orders 是期货专属,CFD 不适用);EngineServices::openOrder 解析 `sl_price`/`tp_price` 参数,**非 cfd 明确拒绝**;REPL `open --sl P --tp P` + MCP schema 两个 optional number
- **测试**:+28(明细见 Test Summary M20 行);McpServer 工具数 17→18 两个测试同步更新
- **部署**:systemd 重启后验收——持仓同步 0(用户手动平仓后重启同步正确)、策略运行中但零下单、信号板随 kline 回填出数据
- **遗留观察**:① 持仓 `open_time_str` 显示为 +8h 错误偏移(1786957606000ms 被当作本地墙钟再加 UTC 标签)——display bug 家族一员,待查 ② 引擎内存持仓快照在外部手动平仓后不会自动清除,需重启触发 startup sync ③ 用户 sell@4418 触发单已不在活跃订单(去向待查 Gate 历史)

### M21 持仓热同步 + 动态 SL/TP (2026-08-17, done — 745 tests, 已提交 6e15245/a0d31e5)
- **背景**:验证单 SL/TP 附件实测通过(交易所持仓记录 price_sl 4391.33/price_tp 4405.33 与发送值一致);但用户**习惯在 Gate App 手动平仓**(盈利差不多就平),引擎视图滞后产生幽灵仓(XAUUSD_Buy_1),重启才清——需求:热同步 + 动态 SL/TP。
- **sync_positions**(MCP/REPL `sync`/JSON-RPC):futures + CFD 对账,导入缺失持仓 + **清幽灵仓**(exchange_position_id 不在最新列表且 age >60s 宽限期才删,防填充延迟误删)。启动、主循环每 ~10s(50 ticks)、手动三路复用同一实现(EngineServices 内部,旧的 main.cpp 自由函数已删;SymbolRegistry 传入 ctor 做 quanto 查找)。
- **modify_sl_tp**(MCP/REPL `modify <id> [--sl P] [--tp P]`):`GateRestClient::putCfdPositionModify` → `PUT /tradfi/positions/{id}` {price_sl, price_tp}(MT5 字符串,"0" 清除);校验顺序:params→id→至少一个字段→持仓存在→仅 CFD→infra;成功后 `updateExchangeStops` 立即刷新本地视图。
- **get_positions**:Position 结构新增 `sl_price`/`tp_price`(0=无,交易所为准),to_json 输出——子代理可直接看到持仓保护价。
- **实测**:引擎重启后 JSON-RPC 直连验证 sync_positions 返回摘要、modify_sl_tp 对未知仓返回 9101、get_positions 空仓正常。
- **子代理现状**:v2 交易模式已授权(按设计文档 §4),验证单 +1.20 USD(用户手动平),已挂起等本小节落地后恢复循环 + trailing 升级。

### Next Steps (2026-08-17)
- ✅ **M20 已提交推送**（d5daf08）
- ✅ **M21 已提交推送**(6e15245/a0d31e5,745 绿)
- ⏳ **子代理恢复循环 + trailing 升级**(通知已就绪:modify_sl_tp 可做移动止损;持仓热同步 10s 内反映用户手动平仓)
- ⏳ **启动黄金自动交易子代理 v2**(用户已确认风控规则,因子决策模型见 xauusd-signal-board-design.md §4):子代理调 MCP(get_signals 读因子 + get_market XAUUSD klines/ticker 自算 + get_positions/get_account/get_risk)盯盘;因子新鲜度 ≤120s;开仓 `open_order` 带 `sl_price`/`tp_price`(CFD 交易所原生保护);规则——单笔 0.01 手、硬止损 -5 USD、止盈 +8~10、日亏 -8 USD 停手、每笔复盘到 /home/joey/1_Code/commit_my_life/0_note/;状态落盘 xauusd-agent-state.json
- ⏳ CFD strategy tune-up for the cost model: 0.06 USDT/0.01 lot buy-only commission + gold storage/swap (利差) — RECORDED in docs/CFD_TRADFI.md + OrderExecutor comment, not yet modeled in PnL/risk
- ⏳ Maker-first verification: testnet first, then small live capital; watch logs "Maker-first attempt registered" / "Maker-first fallback"; consider `order_type = "maker_first"` on a futures instance (e.g. maker_timeout_ms 500)
- ⏳ Loopback port returning awselb responses (suspected Clash TUN hijack of loopback traffic) — can investigate separately
- ⏳ Known display bug: futures position PnL multiplies leverage (SKHY showed -6009 vs exchange -244) — calculateUnrealizedPnl/position sync leverage handling
- ✅ #4 RiskManager TOCTOU — `PositionManager::reserve_notional()` atomic reservation mode, single unique_lock replacing 3 independent shared_locks. `RiskEvalResult` added `reservation_id`; `main.cpp` failure path calls `cancel_reservation()`, success path auto-consumes. 5 new tests.
- ✅ #5 OrderTracker Callback Under Write Lock — "collect inside lock, execute outside lock" pattern: `completion_callback_` in `process_order_update()` and `poll_order_status()` called after unique_lock is released. `set_completion_callback()` protected by lock. Added `test_simulate_ws_update()` / `test_try_shared_lock()` test interfaces. 3 new tests.
- ✅ #6 ProxyTunnel Extraction — 373 lines of network code extracted from `gate_ws_client.cpp` into `proxy_tunnel.hpp/.cpp`. Fixed 2 hidden bugs: (1) `handle_connection` thread changed from `.detach()` to joinable; (2) relay socket/thread registration merged into a single lock_guard scope. Removed 58 lines of dead code (SSL relay overloads). 7 new tests.
- ✅ #7 Risk-gate single evaluation (M14, 2026-08-14) — `onSignal` passes its eval into `placeOrder(req, eval)`; the 1-arg overload evaluates once. Kills the 3002 reject loop (Modified orders were rejected against their own reservation) and the reservation leak.
- ✅ #8 Futures quanto notional (M14) — `OrderRequest.quanto_multiplier`; `reserveNotional(qty, price, quanto)`; `SymbolRegistry` (919 futures contracts) fetched at startup in main.cpp and injected via `setSymbolRegistry()`; reserved notional clamped to budget (kills the 1-ULP overflow deadlock).
- ✅ #9 Symmetric fill tracking (M14) — fills close opposite-direction positions first, then open the remainder (SELL fill with no long now opens a tracked short). `m_reservations` stores `ReservationEntry{reservation_id, request}` so fills open positions with the correct market type/leverage/quanto.

### M14 + CFD Direction (2026-08-14) → M15 Dual-Direction Trading (2026-08-15, done)
- **Product pivot**: trading direction moved from BTC_USDT perpetual futures to **Gate.io TradFi/CFD gold (`XAUUSD`)** — see `docs/CFD_TRADFI.md` (full API survey + phased implementation plan) and OPERATIONAL_GUIDE §10.
- **Verified live**: CFD account registered (`mt5_uid 2017864`), XAUUSD ~4348 USD/oz, 1 lot = 100 oz, min 0.01 lot, leverage 20–500; API key **CFD permission works** (futures permission is a separate product — the 403 on `/futures/*` is expected and irrelevant to CFD).
- **M15 implemented (2026-08-15, 632 tests green)**: `MarketType::Cfd` + `active_market` config; EndpointRouter `/api/v4/tradfi/*`; GateRestClient 11 TradFi methods; OrderExecutor static `buildOrderBody` (MT5 schema, live-verified); OrderTracker REST-poll mode; MarketFeed REST-poll loop (ticker 1s / klines 60s, backfill 500); SymbolRegistry CFD branch; `evaluateCfdOrder` (7101/7102); CFD close `/tradfi/positions/{id}/close`; **direction switching** (`switch_direction`/REPL `switch`/MCP, 17 methods) — pauses old-direction strategies, cancels its open orders, positions stay open; gate `InactiveMarket 3008` (reduce_only exempt); `open_order` defaults to active direction; `get_market` market_type-aware.
- **Engine state**: `active_market = "futures"` in trading.toml; XAUUSD instances auto-paused at startup, **CFD never trades until `switch cfd`**. Strategy compat for CFD: momentum/mean_reversion/supertrend OK; orderbook_scalper unusable (no order-book channel, validator rejects).
- **Operational**: CFD balance was 0.00 on 08-15 (user intent); leftover trigger orders (buy@4295/17511143, sell@4428/17471679) uncancellable while market closed — now superseded by M17 notes. MCP tool count 17; nohup: stdout-only logging (`logs/app.log` may miss lines).

Then: live-verified 2026-08-17 (direction switch, CFD order placement + id resolution — see M17 section)

### M16 Maker-First Orders (2026-08-16, done — commits b6f785f / deb5a1e)
- **Config**: per-instance `order_type = "market"|"post_only"|"maker_first"` + `maker_timeout_ms` (ms; > 0 required for maker_first). Validator rejects non-market order_type on `cfd` — TradFi API only has `price_type: market|trigger`, no post-only.
- **Signal flow**: post_only/maker_first signals place `OrderType::PostOnly` at the exact best bid (buy) / best ask (sell) from `OrderBookManager` (futures + spot WS books; futures ticker bid/ask are hardcoded 0). No book data → maker_first falls back to market; post_only drops the signal (never crosses).
- **Sweep**: main loop calls `order_flow.sweepMakerAttempts()` every 200 ms. Expired attempts: cancel (under rest_mutex) → release reservation but KEEP the entry with `reservation_id = 0` (late Cancelled reports still open partial fills with correct futures metadata; `consumeReservation(0)` is a no-op) → fresh 1-arg `placeOrder` for the **remaining** qty (new token + reservation; direction gate / drawdown re-checked).
- **No-chase**: exchange-rejected post-only never falls back to taker; failed cancel (already filled) never falls back. Partial fill + fallback converge to the intended total.
- `OrderType::MakerFirst` is config-only — requests in flight are always Market/PostOnly/Limit. ctor gained two nullable `OrderBookManager*` (spot/futures).

### Engine Ops (2026-08-16, same session — commits d0b305c / 0472f82 / cc381c9)
- **display_timezone**: `[control] display_timezone` ("local"/"utc"/±HH:MM) → `*_str` time fields in JSON-RPC output + REPL time column (src/core/TimeUtil.hpp).
- **Single instance**: flock on `data/engine.lock`; second engine refuses to start (`PULSE_ALLOW_MULTI_INSTANCES=1` bypasses). systemd user service `pulsetrader.service` in deploy/ (loginctl enable-linger + Restart=on-failure + journald).
- **Startup reconciliation**: `GateRestClient::getFuturesPositions` (filters size=0) + `PositionManager::syncPositionFromExchange` (no limit checks, idempotent, `_sync` position id, keeps exchange open_time/liq/leverage); main.cpp syncs at startup (warn-only on failure). Fixed leverage-string crash: `leverage` is a string ("0") in positions JSON.
- **Limits** (user decision, SKHY manual 5051 notional): maxPositionNotional 500→6000, maxSymbolNotional 300→5500, maxOpenPositions 3→4. Engine now systemd-managed (single instance owns 8081/MCP). New-position budget ≈928 USDT.
- **Origin**: dual-engine incident (two engines each placed 3 BTC shorts, phone showed 6) — root-caused, killed the extra engine, fixed above. 1 futures contract = 0.0001 BTC (quanto_multiplier live-verified); 0.0006 = 6 contracts, phone was right.

## Control Plane (L9, `headless` branch)

- **Single binary** `apps/pulsetrader/pulsetrader`, subcommands: `trade` (default; engine + control socket + embedded REPL when stdin is a TTY), `cli` (remote-attach REPL over control socket), `mcp` (stdio MCP server bridging to control socket; auto-loads trading.toml)
- **Control socket**: TCP 127.0.0.1:8081, newline-delimited JSON-RPC 2.0; `[control]` toml (enabled/bindAddress/port), env `PULSE_CONTROL_PORT`
- **Security**: binds localhost-only, no auth — never expose. MCP mode forces file-only logging (stdout = protocol). REST calls serialized via shared mutex in EngineServices
- **18 methods** (method name = MCP tool name): get_status · get_account · get_positions · get_orders · list_strategies · get_strategy_params · set_strategy_param · open_order · close_position · cancel_order · halt_trading · resume_trading · get_risk · get_market · pause_strategy · resume_strategy · switch_direction · get_signals
- **REPL commands**: status · account|balance · positions · orders · strategies · params <id> · set <id> <param> <value> · open <sym> <buy|sell> <qty> [--type market|limit|post_only] [--price P] [--market spot|futures] [--leverage N] [--reduce-only] [--client-id S] [--sl P] [--tp P] · close <position_id> [qty] [price] · cancel <order_id> · halt · resume · pause <id> · resume-strategy <id> · risk · market <sym> [--levels N] [--klines N] · signals · help · quit
- **New capabilities**: per-strategy runtime pause (`StrategyManager::setPaused`), manual trading halt (`halt_trading`/`resume_trading`), live atomic param get/set; order flow unified in `OrderFlowExecutor` (shared by signal aggregator + manual orders)
- **src/control/**: JsonRpcServer · CommandParser · McpServer · ControlClient · EngineServices · OrderFlowExecutor

## Config Structure

Key files: `src/core/config.hpp` (all structs), `config_loader.cpp` (TOML→struct), `config_validator.cpp` (semantic rules)

```
PulseConfig
├── ExchangeConfig   (apiKey, apiSecret, restBaseUrl, wsUrl, futuresWsUrl, proxyUrl, testnet)
├── LogConfig        (level, logDir, toConsole, toFile)
├── StrategyConfig   (aggregator_threshold, cooldown_sec, signal_only, instances[])
│   └── StrategyInstanceConfig (name, symbol, market_type, leverage, margin_mode, ...)
├── RiskConfig       (maxPositionNotional, maxOpenPositions, maxDailyDrawdown, max_leverage, ...)
│   ├── StopLossConfig  (mode, fixed_pct, trailing_pct, max_hold_seconds)
│   └── TakeProfitConfig (targets_pct[], fractions[])
├── AiConfig         (backend, model, apiKey, heartbeatIntervalSec)
├── ControlConfig    (enabled, bindAddress, port) — `[control]` TOML, PULSE_CONTROL_PORT env
├── SqliteConfig     (enabled, dbPath)
└── symbols[]
```

## Error Code Ranges

| Range | Subsystem |
|-------|-----------|
| 1xxx | Network (timeout, disconnect, WS, auth) |
| 2xxx | Exchange (rate limit, balance, invalid order) |
| 3xxx | Risk (rejected, drawdown, position limit, stops) |
| 4xxx | AI |
| 5xxx | Config |
| 6xxx | Trade Recorder |
| 7xxx | Futures (leverage, margin, liquidation, funding, contract) |
| 9xxx | Internal / Control plane (91xx) |

## Operational Setup

- **Branch**: `main` + `headless` (WebUI removed; control plane added)
- **run.sh**: `./run.sh {trade|cli|mcp|rest|ws|market|strategy|ai|test}`
- **run.sh trade**: auto-loads `trading.toml` if no `--config` specified
- **.env**: `PULSE_NETWORK` (mainnet/testnet), `GATE_MAINNET_*`, `GATE_TESTNET_*`, `HTTPS_PROXY`
- **Git proxy**: `http.proxy` / `https.proxy` = `http://127.0.0.1:7897`
- ⚠️ **Mainnet** — real money at risk when `PULSE_NETWORK=mainnet`
- ✅ **Testnet** — virtual funds when `PULSE_NETWORK=testnet` (futures only)

## Code Conventions

- `.clang-format`: Allman braces, 120 col, 4-space indent
- Naming: PascalCase classes (no underscores), camelCase functions/methods (no underscores), m_camelCase member variables, kPascalCase constants. Pure-data struct fields keep snake_case.
- File naming: filenames match primary class name (e.g., `OrderExecutor.hpp` for `class OrderExecutor`). Multi-type modules keep descriptive names (`config.hpp`, `types.hpp`, `risk_types.hpp`).
- Yoda conditions, mandatory braces, `Result<T>` = `std::variant<T, PulseError>`
- `ExchangeConfig.restBaseUrl` = host only (`https://api.gateio.ws`), path includes `/api/v4`

## Notes

- QuantX (`~/1_Code/QuantX`) has reusable Gate.io code (signing, REST, futures adapter)
- Sub-account recommended for risk isolation (max 10 for VIP0-4, inherit main VIP)
- Futures: USDT-settled only, leverage up to 125x, simultaneous spot+futures via config
