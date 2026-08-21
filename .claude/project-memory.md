# pulseTrader — Project Memory

> Last updated: 2026-08-20 (synced 08-20 events: SNDK v2.1 上扩 + ETH 网格部署 + 黄金代理停摆修复)
> File size: 12535 chars / 25000 chars. Must recalculate and sync this line after updating this file.
> Historical details migrated to `project-memory-archive.md`

## Overview

- **Project**: pulseTrader — C++20 high-frequency scalping framework
- **Repository**: https://github.com/James-Xue/pulseTrader (public, GPL 3.0)
- **Exchange**: Gate.io (REST + WebSocket), single-exchange focus
- **Namespace**: `pulse::` · **Build**: CMake + vcpkg
- **Branches**: `main` (WebUI 时代遗留) + `headless` (控制平面,当前开发主线)

## Architecture (9 Layers, All ✅)

| L1 Exchange | L2 Logging | L3 Market Data | L4 AI Analysis | L5 Heartbeat |
|---|---|---|---|---|
| L6 Strategy | L7 Risk Mgmt | L8 Execution | L9 Control Plane | — |

- Hot path (L1→L3→L6→L7→L8) vs AI background (L4→L5), bridged via `std::atomic`
- Control plane (L9, `headless`): JSON-RPC socket 127.0.0.1:8081 + embedded/remote REPL + stdio MCP server
- Proxy: REST via `CURLOPT_PROXY`; WS via `ProxyTunnel` class
- Credentials: `.env` (`GATE_API_KEY`/`GATE_API_SECRET`), gitignored

## Dependencies

- Core: nlohmann-json, spdlog, fmt, curl, openssl, asio, websocketpp, gtest, toml11
- Optional: sqlitecpp (`-DPULSE_ENABLE_SQLITE=ON`)
- Vendored: websocketpp in `third_party/` (uWebSockets/uSockets removed with the WebUI)
- SQLiteCpp GCC 15 fix: build with `-DCMAKE_CXX_FLAGS="-include cstdint"`

## Current State (M24/M25, 2026-08-19/20)

### 2026-08-18~20 更新(家庭机)

- **08-18 五修复已提交**(本地 HEAD 5daeaae,751 绿):CFD 平仓零痕迹(close 合成 ExecutionReport)、MCP 桥 9104 自动重连(ControlClient)、min_confidence 未 seed 致 momentum/mean_reversion 静默(0.6→0.0)、日志 flush_on(info)、+8h 偏移(TimeUtil local-mode offset 恒 0)
- **origin/headless 已 pull 至 2a227c5(M24)**:M22 方向闸门放宽+minAvailableAfterStopUsd(759 绿)、M23 期货触发单三接口(770 绿)、M24 futures sync 外部余量去重(783 绿);公司机断网未同步,按机器名分流,见记忆 pulsetrader-multimachine-sync
- **08-20 网络实测 + 引擎直连**:关 Clash TUN 后实测真直连可用(spot/time 200@2.4s、XAUUSD ticker 200@1.4s、fx-ws:443 TCP 通、DNS 无污染);三路径对比:TUN fake-IP 0.56s 最快 / 7897 代理 1.56s / 直连 1.4~2.4s。`trading.toml` `proxyUrl=""`,引擎直连运行中;**本次启动无爬行**(同步 REST ~1s;爬行根因未修,方案 A+C 待实施)
- **子代理**:08-18 夜用户令停(引擎+代理+2cron);08-20 引擎已重启(直连),子代理循环未跑

### Test Summary
- **783 tests green** (本机 8-19 实测,M22–M24 全量)
- M23: 触发单 3 + 订单查询 + parser/mcp 更新;M21: sync/modify-sl-tp;M20: SignalBoard 6 + OrderFlow 2 + EngineServices 3;M17: 预算 18;M16: maker-first 22;M15: direction-gate 17

### Milestones (M1–M21 全 ✅,历史细节见 project-memory-archive.md)
- **M1–M5** 九层核心 → **M6** TOML 配置 → **M7** SQLite 落库 → **M8** 合约配置 → **M9** EndpointRouter/WS ping-pong → **M10** 合约行情 → **M11** 合约风控/PnL → **M12** 合约执行+双市场 → **M13** testnet
- **M14** (08-14) 风控三连修:单次求值杀 3002 循环 + quanto 名义 + 成交对称化,595 绿
- **M15** (08-15) 双方向 + CFD:Gate TradFi XAUUSD 黄金,`switch_direction`,632 绿
- **M16** (08-16) maker-first + display_timezone + flock 单实例 + systemd + 启动对账,669 绿
- **M17** (08-17) 分市场预算 + CFD 订单 ID 解析,687 绿
- **M18** (08-17) 实盘落库:trades 3 列迁移 + MarketDataSink/MarketRecorder,703 绿
- **M19** (08-17) CFD 执行链 5 修复(时间窗/列表轮询/feed 过滤),717 绿
- **M20** (08-17) signal-only + SignalBoard + get_signals,731 绿
- **M21** (08-17) sync_positions 热同步 + modify_sl_tp 动态 SL/TP,745 绿
- **M22** (08-18) 方向闸门放宽:手动单 futures/CFD 任意方向可执行(placeManualOrder,spot 仍闸门)+ minAvailableAfterStopUsd 风控,759 绿(用户 7e0cace + 我 bb21832)
- **M23** (08-18) 期货触发单 price_orders 三接口(place/list/cancel_trigger_order)+ list_futures_orders 交易所侧挂单查询,770 绿(09087ae)
- **M24** (08-19) futures sync 外部余量去重:引擎自营成交与 synced 仓同品种同向合并后不再重复计数(UNITREE 实证:交易所 20 张,引擎视图 30);_sync 只存外部余量,零余量删条目;783 绿(c84cd2a)
- **M25** (08-19 定稿) 黄金代理收官:gate_ledger.py 沉淀 + 接力重建(cron */3 + flock);Gate 权威对账净 +3.69(原 +5.88 虚高,转账 3 笔实锤)

### M21 持仓热同步 + 动态 SL/TP (2026-08-17, 6e15245/a0d31e5)
- 背景:用户习惯在 Gate App 手动平仓,引擎视图滞后产生幽灵仓(XAUUSD_Buy_1),重启才清
- **sync_positions**(MCP/REPL `sync`/JSON-RPC):futures + CFD 对账,导入缺失持仓 + 清幽灵仓(exchange_position_id 不在最新列表且 age>60s 宽限期才删);启动、主循环每 ~10s、手动三路复用同一实现
- **modify_sl_tp**(MCP/REPL `modify <id> [--sl P] [--tp P]`):`PUT /tradfi/positions/{id}` {price_sl, price_tp}("0" 清除);仅 CFD;成功后立即刷新本地视图
- **get_positions**:Position 新增 sl_price/tp_price,子代理可直接看持仓保护价
- 实测:SL/TP 附件交易所记录与发送值一致;验证单 +1.20 USD(用户手动平)

### M20 Signal-Only + SignalBoard (2026-08-17, 0041a4a..d5daf08)
- 背景:引擎策略与 LLM 子代理双头交易风险(8-17 17:06 事故:策略自动开 XAUUSD 多 0.01 无保护,浮亏 -7.5 用户手动平);设计文档 `~/1_Code/commit_my_life/0_note/gate交易/黄金/joey-Z170I-PRO-GAMING/策略/xauusd-signal-board-design.md`
- **signal_only 模式**:`[strategy] signal_only = true`(已启用)→ 引擎策略只发信号不下单,手动 placeOrder 不受影响
- **SignalBoard**:每 strategy_id 覆盖式保留最新 Entry(signal + ts_ms),聚合独立槽位;`get_signals` 输出 signals[] + aggregate
- **open_order 附加 SL/TP**:OrderRequest sl_price/tp_price,CFD 分支填 price_sl/price_tp(交易所原生保护,引擎挂了也止损);非 cfd 明确拒绝
- 遗留观察:① open_time_str +8h 偏移显示 bug ② 引擎内存持仓快照外部平仓后需重启/热同步清除

### 2026-08-18 五个修复 (已拉取至 5daeaae,远程 751 绿)
- `f94a74f` TimeUtil local-mode 偏移恒 0 — 墙钟错标 +00:00
- `f36d8cd` CFD 平仓留痕 — 合成 ExecutionReport(平仓零痕迹缺陷)
- `dfdf8a7` ControlClient 传输故障自动重连(MCP 桥重连缺陷)
- `8ad1884` 每实例 seed min_confidence + 恢复动量/均线发信(信号静默)
- `5daeaae` 日志 info 级即刷盘(低流量时段缓冲数小时)

### 家庭电脑环境 (2026-08-17, MECHREVO;本机 joey 同流程)
- 编译:`build_headless`(Debug + SQLITE ON),745 绿 11.7s;旧 `build/` 已废弃
- 配置:trading.toml 主网 CFD 黄金(XAUUSD 3 策略 + signal_only + 风控 6000/5500/4);`.env` 不入库
- MCP 注册:`claude mcp add pulsetrader -- <repo>/run.sh mcp`;MCP 模式 stdout 必须纯 JSON-RPC(曾移除 echo 污染)
- 引擎 systemd 管理(single instance 独占 8081);08-20 已重启(家庭机 nohup 直连,见上节)

## 关键决策与事故 (M14–M16 时代,细节见 archive)

- **CFD 转向 (08-14/15)**:API key 无 futures 权限 → 转 Gate TradFi/CFD 黄金 XAUUSD(~4348 USD/oz,1 lot=100 oz,min 0.01,杠杆 20–500);`docs/CFD_TRADFI.md` 全 API 调研;orderbook_scalper 无 book 频道不可用;`active_market = "cfd"`;CFD 账户曾清零(用户意图)
- **双引擎事故 (08-16)**:两个引擎各下 3 单 BTC 空,手机显示 6 张 — 根因双实例;修复 flock 单实例 + systemd + 启动对账;1 合约 = 0.0001 BTC(quanto 实盘验证)
- **maker-first (08-16)**:post_only 最优价 + 超时市价兜底 + 不回追;**CFD 不支持**(TradFi 仅 market/trigger);省手续费 0.05%/0.02%
- **M17 预算 (08-17)**:maxPositionNotional{Futures,Cfd,Spot} 分市场上限(6000/5500/4 全局);SKHY 合约 5099 吃掉 CFD 预算的根因修复;CFD POST /tradfi/orders 不回显 order id → `matchCfdOrderId` 列表解析
- **M18 落库 (08-17)**:trades 表 +3 列(market_type/leverage/quanto)+ migrateSchema(user_version v1);MarketRecorder(POD 环队列 8192,批量 128/1s,kline INSERT OR IGNORE PK);实盘验证:用户手动平仓后重启同步正确

## Control Plane (L9, `headless` branch)

- **单二进制** `apps/pulsetrader/pulsetrader`,子命令:`trade`(引擎+控制 socket+内嵌 REPL)/`cli`(远程 REPL)/`mcp`(stdio MCP 桥,自动加载 trading.toml)
- **控制 socket**:TCP 127.0.0.1:8081,换行分隔 JSON-RPC 2.0;`[control]` toml,env `PULSE_CONTROL_PORT`
- **安全**:仅 localhost 无认证 — 永不暴露;MCP 模式强制文件日志(stdout=协议);REST 经共享 mutex 串行
- **20 方法** = MCP 工具名:get_status · get_account · get_positions · get_orders · list_strategies · get_strategy_params · set_strategy_param · open_order · close_position · cancel_order · halt_trading · resume_trading · get_risk · get_market · pause_strategy · resume_strategy · switch_direction · get_signals · sync_positions · modify_sl_tp
- **REPL**:status · account · positions · orders · strategies · params · set · open(含 --type/--price/--market/--leverage/--reduce-only/--client-id/--sl/--tp) · close · cancel · halt · resume · pause · resume-strategy · risk · market · signals · sync · modify · help · quit
- **src/control/**:JsonRpcServer · CommandParser · McpServer · ControlClient(自动重连)· EngineServices · OrderFlowExecutor(统一订单流)

## Config Structure

Key files: `src/core/config.hpp` (all structs), `config_loader.cpp` (TOML→struct), `config_validator.cpp` (semantic rules)

```
PulseConfig
├── ExchangeConfig   (apiKey, apiSecret, restBaseUrl, wsUrl, futuresWsUrl, proxyUrl, testnet)
├── LogConfig        (level, logDir, toConsole, toFile)
├── StrategyConfig   (aggregator_threshold, cooldown_sec, signal_only, instances[])
│   └── StrategyInstanceConfig (name, symbol, market_type, leverage, margin_mode, order_type, maker_timeout_ms, ...)
├── RiskConfig       (maxPositionNotional, maxOpenPositions, maxDailyDrawdown, max_leverage, maxPositionNotionalFutures/Cfd/Spot, ...)
│   ├── StopLossConfig  (mode, fixed_pct, trailing_pct, max_hold_seconds)
│   └── TakeProfitConfig (targets_pct[], fractions[])
├── AiConfig         (backend, model, apiKey, heartbeatIntervalSec)
├── ControlConfig    (enabled, bindAddress, port) — `[control]` TOML, PULSE_CONTROL_PORT env
├── SqliteConfig     (enabled, dbPath, recordMarketData)
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

- **run.sh**: `./run.sh {trade|cli|mcp|rest|ws|market|strategy|ai|test}`;trade 自动加载 `trading.toml`
- **.env**: `PULSE_NETWORK` (mainnet/testnet), `GATE_MAINNET_*`, `GATE_TESTNET_*`, `HTTPS_PROXY`
- **Git proxy**: `http.proxy` / `https.proxy` = `http://127.0.0.1:7897`(本机 joey 无 Clash 常驻)
- ⚠️ **Mainnet** — real money at risk when `PULSE_NETWORK=mainnet`
- ✅ **Testnet** — virtual funds when `PULSE_NETWORK=testnet` (futures only)

## Code Conventions

- `.clang-format`: Allman braces, 120 col, 4-space indent
- Naming: PascalCase classes (no underscores), camelCase functions/methods (no underscores), m_camelCase member variables, kPascalCase constants. Pure-data struct fields keep snake_case.
- File naming: filenames match primary class name; multi-type modules keep descriptive names
- Yoda conditions, mandatory braces, `Result<T>` = `std::variant<T, PulseError>` — use `ok()` / `value()` / `error()`, not `has_value()`
- `ExchangeConfig.restBaseUrl` = host only (`https://api.gateio.ws`), path includes `/api/v4`

## Notes

- QuantX (`~/1_Code/QuantX`) has reusable Gate.io code (signing, REST, futures adapter)
- Sub-account recommended for risk isolation (max 10 for VIP0-4, inherit main VIP)
- Futures: USDT-settled only, leverage up to 125x, simultaneous spot+futures via config

## SNDK 网格 v2.1(2026-08-18 部署,08-19/20 上扩)

- **规格 v2.1**:20 格限价空 1615~1710 步进 5(演进:v1 36 格 1730~1800 → v2 12 格 1600~1665),每格 2 张(quanto 0.01,~35 USD/格);只做空;分格 TP = 成交价-10 的 reduce-only 限价买单(price_orders 不支持部分平仓 → 协议 v2);不挂止损;TP 兑现后同格循环重挂;保护线 A = 1730
- **实盘状态(08-19/20)**:旧网格 12/12 全成交(末 1670@20:38,TP 已挂),新 8 格 20:43 挂出(1675/1680 挂出即成交);sync 行 40 张 = 用户 App 加空(均价 1669.11)与网格单 disjoint,**非双计**(Gate 单向持仓模式,App 单与网格单合并);通宵值守 14h/1736 轮(FOMC 1093 轮满分),59 兑现 **+11.8 USD**,铁律 0 违规
- **代理文档**:`~/1_Code/commit_my_life/0_note/gate交易/闪迪/joey-Z170I-PRO-GAMING/`(策略/任务书/思考板/状态);黄金代理同构目录在 `gate交易/黄金/joey-Z170I-PRO-GAMING/`;并行各管各市场
- **经验**:批量挂单 CLI 输出不可靠(报错走 stderr)、maxOpenPositions=4 拦网格(→40)、引擎重启后 tracker 视图丢旧单(用 list_futures_orders 交易所侧视图)、触发单必须 size=2 不能用 auto_size=close

## ETH 网格(2026-08-20 部署,开局暴拉)

- 以闪迪 v2.1 为模板克隆的追空网格,文档在 `~/1_Code/commit_my_life/0_note/gate交易/以太坊/joey-Z170I-PRO-GAMING/`(策略/任务书/思考板/状态)
- **规格(已部署)**:20 格限价空 step 5,每格 2 张(0.01 ETH ≈ 20 USD @2011,~40 USD/格,总名义 ~805);分格 TP = 成交价-10 的 reduce-only 限价买单;无单格止损;保护线 A 顶+20 · B -30 USD;日亏 -10;重锚冷却 30 分钟;ETH maker 费率 -0.01%(挂单返佣)
- trading.toml 已加 ETH 3 策略 signal_only,**maxOpenPositions 40→80**,引擎重启 17 实例
- **首锚 2020**(mid 2012.55),部署即暴拉 +8.9%(2010→2118):**20 格全部成交**,11+ 次 TP 兑现(按成交价精算 **+5.0 USD**),10 格在持 10 格已兑现并循环重挂(2080 格二次成交)
- ⚠️ **9103 名义闸击穿**:2085 格 TP 兑现后重挂空单被拒 → 裸奔无保护;协议"每批探闸 1 笔",用户平仓后闸开
- 固化对账脚本 `gate交易/以太坊/joey-Z170I-PRO-GAMING/工具/gate_eth_state.py`(签名直查 Gate,绕过引擎分页/缓存 bug)
- 通宵值守 51 轮,04:51 清仓 -39.2 停手(三机通宵托管收官)
- ⚠️ 用户 App 活跃:持仓 ~210 + 新挂 -100@2128;用户已有 ETH 手动空仓,**代理铁律不触碰**

## 黄金代理 08-20 停摆与修复

- **事故**:`xauusd-agent-state.json` 第 43 行 2 处裸 ASCII 引号写进 JSON 字符串值 → `json.load` 失败 → relay MODE 为空 → 08-19 22:18Z 起连续 **73 次静默 `skip: mode=`**(stderr 被 `2>/dev/null` 吞)
- **修复**(commit_my_life `3095b7f`):状态机扫描只转义字符串值内部裸引号(2 处),结构引号不动;relay(`~/bin/xauusd-relay.sh`)json.load 失败改 **WARNING 告警** + 错误内容入日志,不再静默跳过
- **当前状态**:接力按用户令稳定停止(mode=stopped,每 3 分钟 tick 仅留 `skip: mode=stopped`);**恢复接力 = 状态文件 `mode` → `running`,下个 tick 自动拉起**

## 笔记目录 (2026-08-19 重组)

`~/1_Code/commit_my_life/0_note/gate交易/` 按市场×机器×类别组织:
黄金 & 闪迪 → 机器(joey-Z170I-PRO-GAMING / james-MECHREVO / ZhangdeMacBook-Pro)→ 任务书/思考板/状态/策略/复盘/统计
- Gate 通用 API 备忘(期货触发单)在 gate交易/ 根目录
- 文档内路径引用已同步更新;续接子代理会话时按新路径读取

## Next Steps (2026-08-20)

- ✅ SNDK 网格首轮实盘验证(通宵 59 兑现 +11.8 USD)✅ 双代理/三市场并行(三机通宵托管收官)
- ⏳ **9103 名义闸 2 处待处置**:ETH 2085 格重挂被拒(裸奔无保护)+ SNDK 9103 闸击穿待拍板;协议"每批探闸 1 笔";modify 锁在列
- ⏳ **黄金代理接力恢复**:状态文件 `mode` → `running` 即可(下个 tick 自动拉起);任务书加"写状态 JSON 转义引号"纪律防复发
- ⏳ **黄金自动交易子代理 v2**(规则已确认,因子决策见 gate交易/黄金/joey-Z170I-PRO-GAMING/策略/xauusd-signal-board-design.md §4):get_signals 读因子 + get_market 自算,新鲜度 ≤120s;单笔 0.01 手、硬止损 -5 USD、止盈 +8~10、日亏 -8 停手;状态落盘 xauusd-agent-state.json;18:00 窗口 XAU 深空 = SHORT 候选
- ⏳ 黄金对账遗留:引擎 position_id 漂移 + 外部裸单沟通 + 当日 realized 权威复核
- ⏳ CFD 成本模型:0.06 USDT/0.01 手佣金 + 黄金库存/swap 利差,未入 PnL/风控
- ⏳ maker-first 实盘验证(先 testnet 后小资金)
- ✅ 显示 bug 已修:futures PnL 乘杠杆(36ee6f9)、open_time_str +8h 偏移(f94a74f)
- ⏳ loopback awselb 响应(疑 Clash TUN 劫持;用户已关 TUN,可能已无关)
- ⏳ 规则调优候选(夜盘纸面 3W/1L +15.7):RSI<30 破位禁追空(4/4 被买回教训)、双收盘+ticker 确认纪律、ATR 动态止损
