# ETH 网格 v2 文字策略 → C++ 实现映射表

> 对照 `策略/eth-subagent-strategy.md`(v2 规范,`docs/gate交易/以太坊/james-MECHREVO/策略/`)与 `src/grid/GridManager.{hpp,cpp}`。
> 状态:✅ 已引擎化 · ⏳ 未转化(维持子代理/工具链) · 🗑️ 已删除
> 最后核对:2026-08-21(M27 PR-6/PR-7,867 绿)

## 规则映射

| # | v2 文字规则(§) | C++ 实现点(src/grid/GridManager.cpp) | 状态 |
|---|---|---|---|
| 1 | 只做空,每格 2 张限价空(§1) | `hangLevels()` — sell limit,`m_cfg.qty_per_level` | ✅ |
| 2 | 锚 A = round(mid + 1×step, step),网格区 = [A, A+12×step](§3.1) | `recomputeLevels()`(anchor_offset_steps=1) | ✅ |
| 3 | step = clamp(round(0.5×ATR15m), 3, 8)(§3.1) | `computeAtr15m()` + `recomputeLevels()`(step_mode="atr") | ✅ |
| 4 | a) mid < 锚-3×step → 下移重锚,30min 冷却(§3.1a) | `decideAction()` Reanchor 分支 + `reanchor()`(lower_reanchor_steps=3) | ✅ |
| 5 | b) mid > 上沿+1×step → **不自动重锚**,转保护线 A(§3.1b) | `upper_reanchor_steps` 字段删除(PR-6),路径走 ProtectA | ✅ 🗑️ |
| 6 | c) 趋势闸门:15m EMA 空头才挂新格/重锚(§3.1c) | `readTrendGate()` — SignalBoard `eth_scalper_ETH_USDT` trend_state,墙钟 ≤120s 新鲜度,stale=冻结 | ✅ |
| 7 | d) 暴拉冻结:1m 涨幅 > max(1%, 3×ATR15m) → 冻结**新格+重锚** 30min(§3.1d) | `spikeTripped()` + `markSpikeIfNeeded()` + decideAction 重锚分支闸门(PR-6 修复) | ✅ |
| 8 | 成交 → 挂 reduce-only 限价买 TP = 成交价-2×step,严禁 place_trigger_order(§3.2) | `manageTp()` — reduce_only limit buy,`tp_distance_steps=2` | ✅ |
| 9 | TP 兑现 → 记账 + mid<格价时循环重挂(§3.3) | `manageTp()` + `recordTpFill()`(realized 记账 + 日亏检查) | ✅ |
| 10 | 保护线 A:1m/15m 收盘 > 顶+2×step → reduce-only 市价平**恰网格份额**+按 §3.1 决定重挂(§4) | `decideAction()` ProtectA + `flattenGridShare()`(share = Σfilled × qty,不超份额;重挂闸门 PR-6) | ✅ |
| 11 | 保护线 B:网格浮亏 ≤ -30 USD → 同 A(§4) | `decideAction()` ProtectB + `flattenGridShare()` | ✅ |
| 12 | 日亏 ≤ -10 停手:不再开新仓/重挂,存量照管;北京 08:00 重置(§4) | `dailyLossFrozen()` + `tickFast()` 日界(UTC 00:00 == 北京 08:00)+ `GridAction::DailyStop`(PR-6 接入) | ✅ |
| 13 | 撤单只限自己 eth-grid-* 前缀(§5/§6) | `cancelAllGridOrders()`(前缀匹配) | ✅ |
| 14 | 交易所侧挂单视图为真相(重启丢 tracker)(§7) | `reconcileExchange()`(IGridGateway::openFuturesOrders) | ✅ |
| 15 | 引擎崩溃恢复:重启后自动续跑 | `loadState()`/`saveState()` schema 2 — phase/tp_resting/geometry 恢复(PR-7);重启首个 slow pass 先对账 | ✅ |
| 16 | 崩溃幸存 TP 单 → 采用不重复挂 | `reconcileExchange()` adopt 分支(按 TP 价匹配 level) | ✅ |
| 17 | 方向切换撤单判"取消"不判成交 | `onDirectionSwitched()` → `m_externalCancelPending` | ✅ |
| 18 | 用户手动仓不触碰;存在非网格仓 → 拒启(force 可越)(§6) | `refreshUserPositionWarn()` + `start()` 预检 | ✅ |
| 19 | 每笔平仓写复盘 md(§1) | ⏳ 未转化 — 引擎 info 日志 + realized 记账代替 | ⏳ |
| 20 | 机械 watchdog 状态 JSON(eth_watch.py) | ⏳ GridManager 取代,eth_watch.py 待退役 | ⏳ |

## 网格控制面(REPL / JSON-RPC / MCP 三通道)

| 命令 | 作用 | 错误码 |
|---|---|---|
| `grid start [--levels N] [--qty Q] [--step S] [--anchor P]` | 启动(覆盖参数永久生效,重启后仍保持) | 9201 已在跑 / 9101 参数非法 |
| `grid status` | 全量快照(phase/anchor/step/top/每格/盈亏/闸门/冻结) | — |
| `grid pause` | 暂停:挂单留交易所,无新动作(落盘) | 9101 未在跑 |
| `grid resume` | 恢复 Paused → Running(下个 slow pass 先对账) | 9202 未暂停 |
| `grid stop` | 停止并撤全部 eth-grid-* 单 | — |

## 参数对照(GridConfig ↔ v2 规范)

| GridConfig | 默认 | v2 规范 | 备注 |
|---|---|---|---|
| levels | 12 | 12 格 | |
| qty_per_level | 0.02 | 2 张(0.02 ETH) | |
| step_mode / step_atr_mult / step_min / step_max | atr / 0.5 / 3 / 8 | clamp(0.5×ATR15m, 3, 8) | |
| atr_period | 40 | ATR15m(1m TR 均值 40 根) | |
| tp_distance_steps | 2.0 | 成交价 - 2×step | |
| anchor_offset_steps | 1.0 | 锚 = mid + 1×step | |
| lower_reanchor_steps | 3.0 | 下沿 - 3×step | |
| protect_line_a_steps | 2.0 | 顶 + 2×step | upper_reanchor_steps 已删 |
| protect_line_b_usd | -30.0 | ≤ -30 USD | |
| daily_loss_limit_usd | -10.0 | ≤ -10 USD | |
| daily_reset_hour | 8 | 北京 08:00 | |
| reanchor_cooldown_min / spike_freeze_min | 30 / 30 | 30 / 30 min | |
| spike_pct / spike_atr_mult | 0.01 / 3.0 | max(1%, 3×ATR15m) | |
| force | false | — | 非网格仓存在时拒启 |

## 引擎化演进史

- **M27 PR-1~5**(fd8505c..f724fbb):GridManager 落地(挂格/TP/重锚/保护线/闸门/持久化 schema 1/控制面四命令)
- **M27 PR-6**(199b043):start 覆盖参数生效、grid_resume、重锚加 spike/日亏闸门、DailyStop 接入、保护线后重挂闸门、删 upper_reanchor_steps、修复 main.cpp tick 编译
- **M27 PR-7**(e023979):持久化 schema 2(phase/tp_resting/geometry 恢复)、pause 落盘、[grid] TOML 示例段
- **待办**:引擎重启生效 → testnet 演练(挂格→成交→TP→循环;保护线;重启;方向切换)→ 主网启用,eth_watch.py 退役
