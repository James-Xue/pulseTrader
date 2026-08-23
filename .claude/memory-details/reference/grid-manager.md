# M27 引擎内网格服务 GridManager (2026-08-21, fd8505c..f724fbb, 848 绿)

> `project-memory.md`「M27 引擎内网格服务」一节的细节展开。

## 动机

ETH 网格 v2 规则(Python 工具链 + LLM 子代理)不可单测、绕开引擎风控、状态易漂移;复盘 eth-review-20260821-grid-v1.md 后 C++ 化

## 架构(`src/grid/`,pulse_grid 库,无 pulse_control 依赖,IGridGateway 抽象防链接环)

- **IGridGateway**(place/cancel/openFuturesOrders/positionsBySymbol 纯虚);生产实现 `GridGateway`(main.cpp 编译进可执行文件):place → **placeManualOrder 全风控**(M22 宽松闸),cancel → 直撤交易所优先(tracker 看不到重启前订单),openFuturesOrders → getFuturesOrders(交易所真相视图)
- **GridManager**:无独立线程,主循环 200ms tick 驱动,内部 fast(每拍 spike/日界)/mid(~1s 趋势/冻结到期)/slow(~50s 对账主流程)分层;m_mutex 守卫(锁序 m_mutex→rest_mutex)
- **规则链**:挂格(锚=round(mid+1×step),ATR 自适应 step=clamp(0.5×ATR15m,3,8))→ 成交整格记账(filled+=qty_per_level)→ TP reduce-only 限价买(fill-2×step,严禁 place_trigger_order)→ TP 兑现记账+循环重挂 → 重锚(下移跟随,冷却 30min+趋势 bearish)→ 保护线 A(1m 收>顶+2×step)/B(浮亏≤-30)→ reduce-only 市价平**恰好网格份额**+重锚 → 趋势闸门(读 SignalBoard eth_scalper trend_state,wall-clock 新鲜度,stale=禁新格)→ 暴拉冻结(1m 涨幅>max(1%,3×ATR15m),冻结期不续刷)→ 日亏停手(realized≤-10,北京 08:00==UTC 00:00 重置)→ 方向切换撤单判"取消"不判成交(externalCancelPending 标志)
- **持久化**:JSON tmp+rename 原子写(data/grid_state.json),重启后交易所视图为真相

## 风险层 reduce-only 语义(PR-1,9103 根因)

`reserveNotional(..., reduce_only, side)`——平仓单跳过 maxOpenPositions 名额;名义只计同 symbol 反向仓之外的 excess;excess≤0 直接 Approved 绝不 Modified(缩减 TP 会半仓裸奔)

## 控制面

`grid_start [--levels N] [--qty Q] [--step S] [--anchor P]` / `grid_status` / `grid_pause` / `grid_stop`(REPL/MCP/JSON-RPC);错误码 GridNotStarted=9200/GridAlreadyRunning=9201;start 预检用户仓(非 eth-grid-* 存在则拒绝,除非 force)

## 踩坑实录

① start/pause/stop 持锁调 status() 自死锁→拆 statusLocked();② SignalBoard JSON 键是 "source" 非 "strategy_id";③ 订单类型判断不能用 order_id 前缀(成交后 client_order_id 丢失)→ map 存 TrackedOrder{idx,is_tp};④ TP 消失分支必须优先于 resting==0 的 sell 分支;⑤ 趋势新鲜度用 wall-clock(测试推进 now_ms 会误过期)

## 待办(M30 后更新)

- ✅ 已随 M30 部署重启生效(2026-08-23)
- ⏳ testnet 演练(挂格→成交→TP→循环;保护线演练;重启演练;方向切换演练)→ 演练通过后启 mainnet 网格,eth_watch.py 退役(eth_ledger.py 保留应急)
