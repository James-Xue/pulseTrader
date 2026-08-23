# pulseTrader — Project Memory

> Last updated: 2026-08-23 (M30 futures K 线停录 + 启动免热机,967 绿;M29 回测;M28 验证)
> File size: 20368 chars / 25000 chars. Must recalculate and sync this line after updating this file.
> 细节文件索引(memory-details/,按主题拆分,主文件只留摘要+指针):
>   reference/gate-kline-api.md — Gate k线 REST 三端点铁律(limit/from-to/格式/新鲜度/黄金 kline_type 坑)
>   reference/backtest-engine.md — M29 回测引擎全量(组件/实测/端到端基准)
>   reference/strategy-architecture.md — M26 策略架构全量(UnifiedScalper/Registry/custom_params/EthScalper v2)
>   reference/grid-manager.md — M27 GridManager 全量(规则链/reduce-only 语义/踩坑实录)
>   sessions/grids-sndk-eth.md — SNDK v2.1 + ETH 网格实盘实录
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

## Current State (M24–M30, 2026-08-23)

### 2026-08-23 M30 futures K 线停录 + 引擎启动免热机 (967 绿, 已部署)

- **动机**:Gate REST 合约 k 线实测几乎实时(最新一根滞后 ~33s),而自录 futures kline 质量差(08-21 脏蜡烛 76381.8 + 12% 成交量失真,交叉验证 sqlite vs REST 直拉 2881 根:价格 2533 根完全一致但 347 根成交量差 >10 倍);用户决定不再自录合约 K 线
- **① 停录**:`MarketRecorder::open()` 第 5 参 `skip_kline_markets`(vector<MarketType>,缺省空=全录),构造时建 bitmask,`onKline` 入口短路(**onTicker 不动** — 用户确认只停 kline 保留 ticker);配置 `[sqlite] skip_kline_markets = ["futures"]`(config_loader 数组解析,非法值报错,缺省兼容旧配置);trading.toml 已启用
- **② 免热机**:新 `src/backtest/WarmupSeeder.{hpp,cpp}`(pulse_backtest 库,引擎二进制已链接):`seed(feed, symbols, market_type, count=500)` 每 symbol 用 **KlineLoader(SQLite-first+API 补缺+sanitize)** 拉尾 500 根 1m → 剔 forming(open_time+60s>now)→ **强制 closed=true**(GateKlineFetcher futures 解析不设 closed,不强制则 strategyLoop 的 closed 门永不通过)→ 升序 push 进 `feed.getKlineBuffer(symbol)`;**cache_writeback 写死 false**(否则每次启动又写回 futures,违背停录);失败 WARN 降级传统 warmup,绝不阻塞启动
- **装配**(main.cpp L1.5 块,在 feed->start() 之前保证 KlineBuffer 单写者):spot+futures 各建 GateKlineFetcher;CFD 跳过(已有 pollLoop 500 根回填);`[strategy] preload_klines = true` 开关(缺省 true)
- **时序原理**:strategyLoop 首轮 poll 见 latest closed && open_time != 0 → onKline → snapshot(201) 满 → 过 warmup 门 → 首个信号;cooldown 不拦(m_lastSignalTimeMs=0)
- **端到端实测**(重启部署):`[L1.5] warmup preload done — 1996 candles seeded`(BTC/SNDK/UNITREE/ETH 各 499,sqlite 272+api 228);重启后零 "Warming up" 日志,3 分钟内 eth_scalper 出真实信号(对比旧引擎等 201 根实时 ≈ 3.4h);kline_bars futures 2.5 分钟冻结 10166 行、ticker_ticks 持续增长;967 绿(+13:recorder 2/config 7/warmup 4;test_warmup_seeder 注意 stub 类改名避免与 test_kline_loader ODR 冲突)
- **遗留**:回测 CLI 的 cache_writeback 仍会写回 futures kline_bars(显式工具,保留);KlineBuffer push 不去重,注入与 WS 首帧边界至多 1 根重复(接受,last_kline_time 防重复信号)

### 2026-08-23 M28 部署验证 + 4 修复 (4aacbe0/9620c61, 903 绿)

- **验证流程**:systemd user 服务启动(非系统级,unit 在 ~/.config/systemd/user/)→ `switch futures`(方向闸门,15 恢复 4 CFD 暂停)→ get_signals 全链路核验。注意:引擎重启后回 cfd 方向,ETH 策略 paused,必须再 switch futures
- **同族缺陷 4 处**(验证中发现,已修):
  ① **emitSignal 置信度闸丢弃 Flat 状态信号**(conf=0<0.6):eth_scalper v2"每 candle 发布 trend_state/spike"契约从未生效——日志在闸前打印(logSignal 先于 emitSignal),板上永远无条目 → M27 GridManager 趋势闸门数据源失效。聚合器已忽略 Flat,放行零风险。修:StrategyManager.cpp Flat 例外
  ② **eth_scalper 真信号过不了 0.6 闸**:|EMA9-EMA21|/ATR 实测 ~0.03,scale 1.0 恒被丢(11:15 交叉 conf=0.0326 实证)。修:trading.toml eth_min_confidence_scale 1.0→20.0(实测 conf 1.0 上板,type=sell)
  ③ **order_quantity 未播种**:get_strategy_params 恒显默认 0.001(自动路径实际用配置 20 合约,热调无效)——与 8ad1884 min_confidence 同款。修:main.cpp 注册块补播种(现显示 20.0)
  ④ **ema_resonance 同族**:首信号 |ema7-ema200|/ATR=0.5459 不过 0.6 闸被丢。修:trading.toml res_conf_scale 1.0→2.0(验证:conf 1.0 上板,indicators ema7/14/30/60/200 严格递增 + resonance=bull_aligned)
- **903 绿**(新增 EmitSignalFlatBypassesConfidenceGate);提交 4aacbe0(代码)+ 9620c61(文档/示例)
- **板上覆盖式设计**:每 strategy_id 只保留最新条目,eth_scalper 真信号最多 60s 后被下一根 Flat 覆盖(设计如此,消费方按状态读取;共振策略无 Flat,条目保留到下次迁移)
- **引擎当前状态**:running(新二进制含全部修复),futures 方向,ema_resonance/eth_scalper 均 warmup 完成出信号中;auto_trade 全部 0(仅信号)
- **隐私扫描(08-23)**:run.ps1(WebUI 时代 Windows 遗留,引用已删 build/Release 与 WebUI)硬编码 Gate **测试网** key/secret,公开仓库历史可见(c9efae9 起)——已移除改环境读取(aaac867);mainnet 密钥确认从未入库(内容+历史);AI key 仅 env 引用;sk- 命中均为 CSS 类名误报;**待办:Gate 后台轮换测试网密钥**(历史仍可检索,虚拟资金风险低,不值得 filter-repo 重写)
- **文档补齐(08-23)**:新增 docs/strategies/eth-scalper.md(4af38de)——6 个注册策略文档 6/6 全覆盖;README 索引同步;文档含 v2 状态通道/三口径暴拉过滤/scale 20 语义/2026-08-23 实盘经验

### 2026-08-23 工具:批量 k线下载 fetch_klines (b2683c6,已推送)

- **tools/fetch_klines.py**(公开 REST,无需 key):BTC/ETH 合约+现货 + XAUUSD 1m k线 → `data/klines/*_1m.csv`;`python3 tools/fetch_klines.py [hours]`(默认 48h);`data/` gitignore,提交需 `git add -f`
- **Gate k线 REST 三端点铁律**(limit 上限/from-to 互斥/响应格式/新鲜度/黄金 kline_type 坑)→ 详见 `memory-details/reference/gate-kline-api.md`(2026-08-23 新增黄金必须显式 `kline_type=1m`,否则 INVALID_ARGUMENT)
- M29 回测引擎已落地(见下节),fetch_klines 数据可与 kline_bars 互补

### 2026-08-23 M29 回测引擎(单策略 MVP,5 提交,954 绿)

- **用法**:`./run.sh backtest --strategy ema_resonance_scalper --symbol ETH_USDT --from 2026-08-21 --to 2026-08-23 --quantity 20 --quanto 0.01 [--json x.json]`;窗口缺省自动解析;quanto 内置 ETH 0.01 / BTC 0.0001;错误路径非零退出(缺参 2/未知策略 1/坏时间 2)
- **架构/组件/实测经验/端到端基准**(IKlineSource/SqliteKlineReader/GateKlineFetcher/KlineLoader/ReplayDriver/BacktestAccount/Report;limit 与 from-to 互斥;脏数据净化;迁移触发)→ 详见 `memory-details/reference/backtest-engine.md`
- **后续**:多策略聚合、MCP backtest 方法、orderbook_scalper、参数优化、intra-bar SL/TP、资金费率

### 2026-08-18~20 更新(家庭机)

- **08-18 五修复已提交**(本地 HEAD 5daeaae,751 绿):CFD 平仓零痕迹(close 合成 ExecutionReport)、MCP 桥 9104 自动重连(ControlClient)、min_confidence 未 seed 致 momentum/mean_reversion 静默(0.6→0.0)、日志 flush_on(info)、+8h 偏移(TimeUtil local-mode offset 恒 0)
- **origin/headless 已 pull 至 2a227c5(M24)**:M22 方向闸门放宽+minAvailableAfterStopUsd(759 绿)、M23 期货触发单三接口(770 绿)、M24 futures sync 外部余量去重(783 绿);公司机断网未同步,按机器名分流,见记忆 pulsetrader-multimachine-sync
- **08-20 网络实测 + 引擎直连**:关 Clash TUN 后实测真直连可用(spot/time 200@2.4s、XAUUSD ticker 200@1.4s、fx-ws:443 TCP 通、DNS 无污染);三路径对比:TUN fake-IP 0.56s 最快 / 7897 代理 1.56s / 直连 1.4~2.4s。`trading.toml` `proxyUrl=""`,引擎直连运行中;**本次启动无爬行**(同步 REST ~1s;爬行根因未修,方案 A+C 待实施)
- **子代理**:08-18 夜用户令停(引擎+代理+2cron);08-20 引擎已重启(直连),子代理循环未跑

### Test Summary
- **954 tests green** (本机 8-23 实测,M22–M29 全量)
- M28: EmaResonance 6 + registry 1 + engine services 3 + command parser 1;M27: 风险 reduce-only 5 + 配置 9 + tracker 2 + GridManager 10 + parser 3 + MCP 更新;M23: 触发单 3 + 订单查询 + parser/mcp 更新;M21: sync/modify-sl-tp;M20: SignalBoard 6 + OrderFlow 2 + EngineServices 3;M17: 预算 18;M16: maker-first 22;M15: direction-gate 17

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
- **M26** (08-21) 策略架构:UnifiedScalper 基类 + StrategyRegistry 兜底 + custom_params 通道 + EthScalper 币种策略示范,817 绿(ff616b6/8fb4021)
- **M26.1** (08-21) ETH 网格复盘升级:EthScalper v2 趋势闸门状态(trend_state/spike)+ 三口径暴拉过滤;futures 幽灵仓剪枝;9103 预算 12000/6500(341e0bf)
- **M27** (08-21) 引擎内网格服务 GridManager:ETH 网格 v2 规则 C++ 化(挂格/TP 循环/重锚/保护线 A+B/趋势闸门/暴拉冻结/日亏北京日);reduce-only 名义语义(9103 根因);[grid] TOML 段;控制面 grid_start/status/pause/stop;IGridGateway 抽象,848 绿(fd8505c..f724fbb)
- **M28** (08-23) ① EmaResonanceScalper:五周期 EMA 7/14/30/60/200 严格全排列对齐共振(递增=Bull Buy/递减=Bear Sell/mixed 无信号),共振状态迁移触发(不重发、direct flip 也触发),置信度 = clamp(|ema7-ema200|/ATR,0,1)×res_conf_scale;每根 K 线全量重算(prev=0.0 SMA 播种,确定性无滚动状态);klineNeeded=warmup=201;custom_params 静态(res_ema_p1..p5/res_conf_scale);注册键 ema_resonance_scalper,仅 ETH_USDT futures;规则文档 docs/strategies/ema-resonance-scalper.md(用户要求:每策略必须有文本规则+对应 C++ 实现,已入全局记忆);898 绿(e ea0651)② **按策略一键自动交易开关**:StrategyParams 加 atomic auto_trade(0=仅发信号到信号板永不下单,1=参与聚合走风控下单);闸门在 main.cpp strategy→aggregator wiring(聚合输出丢策略身份,onSignal 无法区分);`[strategy] signal_only` 语义迁移为启动默认种子(重启回仅信号, fail-safe);onSignal 全局拦截删除;接口:MCP set_strategy_trading / REPL `autotrade <id> on|off` / set auto_trade 0|1(带 bounds+审计);list_strategies 带 auto_trade;902 绿(190e84c);③ AGENTS.md 规则:每次 commit 后必须立即 push(e7a0bb5);README/AGENTS/OPERATIONAL_GUIDE 同步(方法数统一 32、6 策略、902 测试)(fe6a38f)

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
- 引擎 systemd 管理(single instance 独占 8081);8-21 状态 **running**(uptime 36h+,CFD 方向,17 策略,新二进制含 M26.1/M27 待重启生效)

### M26 策略架构:UnifiedScalper + Registry 兜底 + 币种策略 (2026-08-21, ff616b6/8fb4021)

- **UnifiedScalper** 模板方法基类(onKline/onTick final;momentum/mean_reversion/supertrend 已迁移,**行为逐字节等价**;orderbook_scalper 独立);**StrategyRegistry**(未注册名→被动 UnifiedScalper 永不发信号+WARN);**custom_params**(TOML 内联表,严格校验);**EthScalper v2**(每 candle 状态信号 trend_state/spike + 三口径暴拉过滤);幽灵仓剪枝;加新策略套路 → 详见 `memory-details/reference/strategy-architecture.md`

### M27 引擎内网格服务 GridManager (2026-08-21, fd8505c..f724fbb, 848 绿)

- **GridManager**:主循环 200ms tick 驱动(fast/mid/slow 三层),规则链=挂格→TP reduce-only 限价买循环→重锚→保护线 A/B→趋势闸门→暴拉冻结→日亏停手;IGridGateway 抽象(GridGateway 走 placeManualOrder 全风控);reduce-only 名义语义(9103 根因);控制面 grid_start/status/pause/stop(9200/9201);5 条踩坑实录 → 详见 `memory-details/reference/grid-manager.md`
- **状态**:已随 M30 部署重启生效(08-23);待办 testnet 演练后启 mainnet 网格,eth_watch.py 退役

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
- **32 方法** = MCP 工具名:get_status · get_account · get_positions · get_orders · list_strategies · get_strategy_params · set_strategy_param · **set_strategy_trading** · open_order · close_position · cancel_order · halt_trading · resume_trading · get_risk · get_market · pause_strategy · resume_strategy · switch_direction · get_signals · sync_positions · modify_sl_tp · get_param_history · get_strategy_performance · grid_start/status/pause/resume/stop · place/list/cancel_trigger_order · list_futures_orders
- **REPL**:status · account · positions · orders · strategies · params · set · **autotrade <id> on|off** · open(含 --type/--price/--market/--leverage/--reduce-only/--client-id/--sl/--tp) · close · cancel · halt · resume · pause · resume-strategy · risk · market · signals · sync · modify · trigger * · grid * · help · quit
- **src/control/**:JsonRpcServer · CommandParser · McpServer · ControlClient(自动重连)· EngineServices · OrderFlowExecutor(统一订单流)

## Config Structure

Key files: `src/core/config.hpp` (all structs), `config_loader.cpp` (TOML→struct), `config_validator.cpp` (semantic rules)

```
PulseConfig
├── ExchangeConfig   (apiKey, apiSecret, restBaseUrl, wsUrl, futuresWsUrl, proxyUrl, testnet)
├── LogConfig        (level, logDir, toConsole, toFile)
├── StrategyConfig   (aggregator_threshold, cooldown_sec, signal_only=启动时所有实例 auto_trade 的默认种子, instances[])
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

## 网格实盘实录(SNDK v2.1 + ETH,2026-08-18~20)

- **SNDK v2.1**:20 格空 1615~1710 步进 5,每格 2 张,分格 TP=成交价-10 reduce-only 买(price_orders 不支持部分平仓→协议 v2);14h/1736 轮 59 兑现 **+11.8 USD** 铁律 0 违规;sync 40 张=用户 App 加空与网格 disjoint 非双计;经验:批量挂单 CLI 输出不可靠、maxOpenPositions=4 拦网格(→40)、重启后 tracker 丢旧单用 list_futures_orders、触发单必须 size=2
- **ETH 网格**:SNDK v2.1 模板克隆,20 格空 step 5 每格 2 张,保护线 A 顶+20/B -30,日亏 -10,ETH maker -0.01% 返佣;首锚 2020 部署即暴拉 +8.9% **20 格全成交** +5.0 USD;⚠️ 9103 名义闸击穿(2085 格重挂被拒裸奔)→ M27 reduce-only 语义修复;通宵 51 轮 04:51 清仓 -39.2 停手
- 全文(规格/演进/对账脚本 gate_eth_state.py/代理文档路径)→ 详见 `memory-details/sessions/grids-sndk-eth.md`

## 黄金代理 08-20 停摆与修复

- **事故**:`xauusd-agent-state.json` 第 43 行 2 处裸 ASCII 引号写进 JSON 字符串值 → `json.load` 失败 → relay MODE 为空 → 08-19 22:18Z 起连续 **73 次静默 `skip: mode=`**(stderr 被 `2>/dev/null` 吞)
- **修复**(commit_my_life `3095b7f`):状态机扫描只转义字符串值内部裸引号(2 处),结构引号不动;relay(`~/bin/xauusd-relay.sh`)json.load 失败改 **WARNING 告警** + 错误内容入日志,不再静默跳过
- **当前状态**:接力按用户令稳定停止(mode=stopped,每 3 分钟 tick 仅留 `skip: mode=stopped`);**恢复接力 = 状态文件 `mode` → `running`,下个 tick 自动拉起**

## 笔记目录 (2026-08-19 重组)

`~/1_Code/commit_my_life/0_note/gate交易/` 按市场×机器×类别组织:
黄金 & 闪迪 → 机器(joey-Z170I-PRO-GAMING / james-MECHREVO / ZhangdeMacBook-Pro)→ 任务书/思考板/状态/策略/复盘/统计
- Gate 通用 API 备忘(期货触发单)在 gate交易/ 根目录
- 文档内路径引用已同步更新;续接子代理会话时按新路径读取

## Next Steps (2026-08-23)

- ✅ M28 两个功能已提交并推送(eea0651 共振策略 / 190e84c 一键开关 / e7a0bb5+fe6a38f 文档与规则)
- ✅ **M28 部署验证完成**(08-23):① 引擎启动 auto_trade=0 种子 ✅ ② get_signals 板上共振信号 + indicators(ema7/14/30/60/200 严格递增 + resonance=bull_aligned)✅ ③ 验证过程发现并修复 4 处同族缺陷(见 Current State 08-23 节,4aacbe0/9620c61)
- ⏳ **autotrade 开关(用户 08-23 决定:暂缓开启)**:验证已完成且信号持续正常,但 08-23 下午为震荡市、共振信号 ~2-5 分钟频繁翻转,用户决定暂不开,保持仅信号;开启时 `autotrade ema_resonance_scalper_ETH_USDT on`(当前 futures 方向已就绪),重启回仅信号(fail-safe)
- ⏳ **M27 引擎内网格服务已落地(5 PR,848 绿),待重启+testnet 演练**:grid_start/status/pause/stop 控制面命令;GridManager 状态机(挂格/TP 循环/重锚/保护线 A+B/趋势闸门/暴拉冻结/日亏北京日);IGridGateway 抽象(GridGateway 生产实现走 placeManualOrder 全风控);reduce-only 名义语义修复(9103 根因);[grid] TOML 段;重启后 get_signals 可见 eth_scalper_ETH_USDT(含 trend_state/spike)+ 15 个 ETH 幽灵仓清理 + 预算 12000/6500 一并验收;ETH 网格 v2 文档与 watchdog 已升级(commit_my_life 以太坊目录),引擎内网格启用后 eth_watch.py 退役
- ⏳ **9103 名义闸 2 处待处置**:ETH 2085 格重挂被拒(裸奔无保护)+ SNDK 9103 闸击穿待拍板;协议"每批探闸 1 笔";modify 锁在列
- ⏳ **黄金代理接力恢复**:状态文件 `mode` → `running` 即可(下个 tick 自动拉起);任务书加"写状态 JSON 转义引号"纪律防复发
- ⏳ **黄金自动交易子代理 v2**(规则已确认,因子决策见 gate交易/黄金/joey-Z170I-PRO-GAMING/策略/xauusd-signal-board-design.md §4):get_signals 读因子 + get_market 自算,新鲜度 ≤120s;单笔 0.01 手、硬止损 -5 USD、止盈 +8~10、日亏 -8 停手;状态落盘 xauusd-agent-state.json;18:00 窗口 XAU 深空 = SHORT 候选
- ⏳ 黄金对账遗留:引擎 position_id 漂移 + 外部裸单沟通 + 当日 realized 权威复核
- ⏳ CFD 成本模型:0.06 USDT/0.01 手佣金 + 黄金库存/swap 利差,未入 PnL/风控
- ⏳ maker-first 实盘验证(先 testnet 后小资金)
- ✅ 显示 bug 已修:futures PnL 乘杠杆(36ee6f9)、open_time_str +8h 偏移(f94a74f)
- ⏳ loopback awselb 响应(疑 Clash TUN 劫持;用户已关 TUN,可能已无关)
- ✅ **M29 回测引擎完成**(08-23,5 提交已推送,954 绿):用法 `./run.sh backtest --strategy X --symbol Y [--from/--to/--quantity/--quanto/--json]`,详见 Current State M29 节;待办:多策略聚合、控制面 MCP backtest 方法、orderbook_scalper、参数优化
