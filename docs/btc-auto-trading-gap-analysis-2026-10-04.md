# BTC 无人值守自动交易 — 就绪度差距分析

> 日期: 2026-10-04 · 分支: `headless` · 基准: 仓库 HEAD `7f0d072` + VPS 只读实况核查
> 方法: 12 个分析代理编队交叉验证 —— 5 个领域读者(策略 / 风控执行 / 行情连接 / 控制面运维 / 研究证据)
> → 3 个差距分析师(工程就绪度 / 策略边际 / 无人值守安全)→ 3 个对抗性验证者(逐条回代码求证)
> → 1 个完备性批评者。结论: 安全维度 19/19 条主张证实; 工程 17 确认 / 1 纠正; 边际 11 确认 / 1 存疑。
> VPS 侧仅做只读核查(引擎状态 / 磁盘 / 文件清单), 未做任何变更。

## 标尺(本报告的"完成"定义)

**无人值守 · 真钱 · BTC_USDT 期货 · Gate 主网 · VPS 部署**: 信号 → 自动下单 → 风控 →
持仓管理 → 监控告警全链路无需人工盯盘。"能跑起来"与"有正期望边际"分开评估。

## 结论

**离"能自动下单"只差一个配置开关; 离"敢让它无人值守用真钱跑"差 8 个 blocker 级工程缺口
(约 1–2 周密集修复 + 2–4 周演练/试跑); 离"值得跑"差一整轮研究 —— 目前 0/6 个在跑策略
有含费验证工件, 唯一做过全周期检验的策略已被证伪。**

| 维度 | 就绪度 | 一句话 |
|---|---|---|
| 链路能跑 | 高(只差部署 + 开关) | 信号→聚合→风控→下单→跟踪→对账全链路闭合, 1045 测试绿 |
| 无人值守安全 | 低 | 止损/熔断/告警/看门狗/kill switch 五条保险丝全未接线或带缺陷 |
| 策略边际 | 零证据 | 在跑策略没有一份含费回测工件; 唯一全周期验证的 IronTrader 已证伪 |

⚠️ **关键口径: 现行 VPS 部署的实盘里程 = 0。** trades.db 里 409 笔成交全部早于本次部署
(2026-08-14 ~ 09-02 旧时代); 本次部署全程 `signal_only=true`, 只验证过"信号计算"分支 ——
下单、私有 WS 成交回报、对账、halt 对平仓的影响等分支, 在真实订单流下从未被触发过。

## 一、现状盘点(底座是扎实的)

- 9 层架构完整; 7 个内置策略 + Registry 兜底; BTC_USDT 已配置 6 个启用实例
  (momentum / orderbook / mean_reversion / supertrend / eth_scalper / ema_resonance)+ 1 个禁用(iron_trader)
- 信号→订单链路闭合: 聚合器(阈值 0.6 / 冷却 30s)→ auto_trade 闸 → 方向闸 → 风控 → OrderFlowExecutor
- 控制面 32 方法(MCP / JSON-RPC / REPL 同名同步); 单实例 flock; 启动 + 每 ~10s 持仓对账
- M33 回测引擎 + 信号日志 + 纸面重放工具链; 币安 7 年全史镜像数据; 每策略文本规则文档
- 近期 WS 重连修复 `c37364b`(公共频道按 (channel,payload) 全量重订)已提交但**未部署**

问题不在"缺功能", 而在**最后一公里接线**与**无人兜底**。

## 二、8 个 blocker(翻 `signal_only=false` 之前必须全修)

### B1. 实盘没有自动退出/止损链路(最致命)

- 期货入场成交后**不挂任何交易所侧止损**: `src/execution/OrderExecutor.cpp:183-215`(futures body 无 price_sl/price_tp,
  仅 CFD 有 `:238-248`)
- 策略的 Flat 退场信号被直接丢弃: `src/control/OrderFlowExecutor.cpp:216-219`
- `StopLossEngine` / `TakeProfitEngine` 是死代码(全库仅 `tests/unit/risk/` 引用);
  `[risk.stop_loss]` `[risk.take_profit]` 配置被解析但无执行侧消费者
- Gate `price_orders` trigger 仅手动 MCP 工具(`EngineServices.cpp:957-1024`), 且只支持全仓 close(size 必须 0)

→ 持仓后唯一的自动退出路径是"反向开仓信号"。**引擎一死/断流, 仓位裸奔, 强平是唯一兜底。**

修复(days): 先探针验证 BTC_USDT 的 `price_orders` 全仓语义与多空合并仓误伤 → 成交后自动挂
reduce-only trigger SL(引擎死亡仍生效); 再把 StopLoss/TakeProfitEngine(含 trailing/max_hold)接线为引擎内第二层。

### B2. 日亏/回撤熔断完全空转

- `DrawdownGuard.updateEquity / resetDaily` 无任何生产调用者(`src/risk/DrawdownGuard.cpp:47-104,150-166`,
  全库 grep 仅命中 tests)→ 配置的**日亏 2% / 峰值回撤 5% 永远不可能触发**(`trading.toml:384-385`)
- `OrderFlowExecutor.cpp:704` recordPnl 只累计不判定; halt 实际只有手动入口
- 数据源已具备: `main.cpp` 60s 心跳已在取 futures balance.total, 只差接线

修复(hours): 心跳里喂 `updateEquity` + UTC 日切 `resetDaily`; 明确触发动作(停新单 + 撤开仓挂单, 可选减仓)。

### B3. 零告警 + 零看门狗

- 全库无任何出站通知实现(Telegram/webhook/邮件均无); `docs/OPERATIONAL_GUIDE.md:104` 列为 P2 待办;
  `docs/architecture.md:147` 的 AlertManager 是文档虚构
- 心跳只写本地日志(`apps/pulsetrader/main.cpp:1612-1628`); systemd 单元 `WatchdogUSec=infinity`、`OnFailure=` 空
  (VPS 实测), `Restart=always/5s` 只对"进程退出"生效 —— **进程"假活"(活着但卡死/僵死)无人能发现**
- 磁盘硬期限: engine.out 实测 **178MB/天**, VPS 余量 5.8G ≈ **32 天写满**(写满即静默停机)
- 先例已发生: A/B 实验 13 天断流空转无人察觉

修复(days): 一条出站通道覆盖事件清单(进程退出/重启、halt、WS 断流、拒单风暴、日亏水位、强平距离);
systemd `OnFailure=` 钩子; 外部 dead-man's-switch(healthchecks.io 或自建)。

### B4. kill switch 有硬伤

- halt 检查在 reduce_only 豁免**之前**(`src/risk/RiskManager.cpp:50-61` vs `PositionManager.cpp:707-750`)
  → **halt 后连平仓单都会被拒**(futures `close_position` 走同一闸门, `EngineServices.cpp:866-877`)
- halt 不撤挂单(停牌期间挂单照常成交)、状态不落盘、无 `cancel_all` / `panic` 方法
  (`cancelAllOpenOrders` 仅被 switch_direction 调用)

修复(days): halt 对 reduce_only/平仓豁免; 新增幂等 cancel_all_orders 与 panic(flatten+halt);
halt/原因落盘 + 启动恢复语义(非正常退出 → 默认停牌 + 告警)。

### B5. 私有 WS 重连丢鉴权 + 订单无周期对账

- `c37364b` 只修了公共频道: 重连 replay 走 `buildResubscribeMsgs` **不带 auth**
  (`src/exchange/GateWsClient.cpp:486-498` vs `:160-175`); `ChannelEntry` 无 private 标记(`GateWsChannels.hpp:147-151`)
- `OrderTracker.m_wsSubscribed` 一次性置位(`OrderTracker.cpp:83-91`); `reconcileAll` 仅在控制面查询 /
  方向切换时触发, 无主循环周期对账

→ **成交回报可静默丢失**, 持仓/在途订单视图长期漂移而无人察觉。

修复(days): 重连补 auth block + 重连成功后强制全量对账 + 主循环 10-30s 低频订单对账 + 回归测试。

### B6. 行情僵死不可检测 + `kline.closed` 恒 true

- WS 不记录最后收包时间、无读超时、从不主动 ping → **半开 TCP 死连接永不触发重连**(`GateWsClient.cpp:528-630`)
- 策略/聚合器不检查数据时间戳(`SignalAggregator.cpp:30-63,96-154` 完全不看 signal.timestamp;
  OrderBookScalper 每 500ms 轮询同一快照不查 `OrderBook.timestamp`)
- `MarketFeed.cpp:517` 把每条推送**硬编码 `closed=true`**(交易所实际推形成中 K 线, 抓帧样例含 `"w":false`)
  → 策略可能拿未收盘数据当终值自动下单, 且与回测(完整收盘 K)系统性偏离

修复(days): 每频道 last_msg_ts + watchdog(N 秒无消息强制重连); 行情 staleness 闸进策略/风控;
解析 `w` 字段修 closed 语义; KlineBuffer 同 open_time 替换而非堆积。

### B7. 下单无幂等

- REST 层对 POST 超时/5xx **自动重试**(`src/exchange/GateRestClient.cpp:284-318`), 而自动信号路径
  **不生成 client_order_id**(`OrderFlowExecutor.cpp:129-208`; 手动/网格路径有, `OrderExecutor.cpp:280-284`)
- → POST 已被交易所接受但响应超时时, 重试 = **重复建仓**(双倍敞口), 且两笔都占用风险预算

修复(hours): 自动路径生成唯一 client_order_id → 重发前按该键查单去重; 或下单类 POST 关闭自动重试。

### B8. 信号→成交→绩效归因断链

- `buildRequestFromSignal` 不写 client_order_id → `recordCompletedTrade` 用空 strategy_id 回退到
  **"该 symbol 第一个 enabled 实例"**(BTC 即 momentum_scalper), `OrderFlowExecutor.cpp:794-799,821-840`
- 影响: `get_strategy_performance`、AI 调参输入、未来单策略熔断全部建立在脏数据上

修复(days): 归因字段贯通 + 补单测钉住多策略同 symbol 场景。

## 三、运维/部署/卫生缺口(非 blocker, 上线前必清)

| # | 事项 | 现状(2026-10-04 实测) |
|---|---|---|
| O1 | VPS 二进制落后 28 天 | 停在 09-06 构建, **缺 `c37364b` 及 09-06 10:05 之后全部提交**; 引擎 inactive+disabled, 恢复需 sudo |
| O2 | 上线配置未裁剪 | VPS 配置 4 品种 17 个 enabled 实例; 名义预算 12000/6500 按旧网格峰值标定 → 须裁到 BTC 单品种 + 按试点资金重定 |
| O3 | 密钥/权限卫生 | 两处 `.env` 均 664(others 可读, 含 OPENAI_API_KEY); key 权限/白名单从未现场核验; 控制口 127.0.0.1:8081 **零鉴权**(本机任意进程可下单) |
| O4 | 数据无灾备 | trades.db VPS 971MB / 本机 487MB, 无备份/归档/quick_check; signals/ticker_ticks 无保留策略 |
| O5 | 双引擎无 leader | 本机与 VPS 各持主网 key; sync 导入交易所全部 futures 持仓无过滤 → 两引擎同跑会互写"外部平仓"污染账本 |
| O6 | 运行期状态不落盘 | halt/peak/日亏/auto_trade/active_market 全为内存态; set_strategy_param 改参与 ParamChangeLog 均为内存 → 重启静默回退 |
| O7 | 对账盲区 | 期货**部分减仓不可见**(`PositionManager.cpp:70-92` 只下修 _sync 不下修 fill-tracked)→ 引擎敞口高估、幻影仓位占预算; 外部平仓 PnL 用最后 mark 估价且 fees=0 |
| O8 | 风控前置校验未接实盘 | 杠杆/保证金校验(`RiskManager.cpp:97-140`)仅被单测调用, leverage 直传交易所; 强平距离只存不判(`risk_types.hpp:100`); 资金费不进 PnL/风控 |
| O9 | 参数口径错位 | 参数边界表按"基础币"标定 vs 合约张数口径; per-strategy stop_loss_pct/take_profit_pct 无执行消费者(操作者/AI 以为已设止损, 实际无行为) |
| O10 | 主循环阻塞面 | 单线程 + 全局 rest_mutex(下单/持仓同步/心跳/网格共用), REST 超时 10s×3 重试 → 最坏锁内阻塞 ~40s |
| O11 | 聚合器残差锁死 | 加权和只在发射后清零、无时间衰减 → 混合流下反向残差可长期"闸死"共识发射, 且无"零共识时长"健康指标(`docs/ab-weight-trial.md` §2 自述) |
| O12 | trades 表脏数据 | 存在量级不可能记录(如 BTC 一笔 pnl=+44,920, 为物理上限 ~28 倍)待审计 |
| — | 升级链 | deploy 脚本以 `kill -9` 触发自愈升级, 按"零仓信号模式"设计; 带仓升级/回滚/演练流程缺失; root unit 与 runbook 不在仓库 |

**一条纠正**: 初稿曾称"VPS 的 AI 心跳在按伪绩效调参" —— 被对抗验证**证伪**: 实际变量名是
`OPENAI_API_KEY` 且 `[ai].apiKey=from_env:AI_API_KEY` 解析为空, engine.out 从 09-04 起每次启动都打
`AI pipeline disabled (no API key or interval=0)` —— **AI 从未通电**。归因断链属实, 但影响限于统计/审计。

## 四、策略边际: 真正的深水区("值不值得"由它决定)

- **0/6 个在跑 BTC 策略有含费回测工件**; `results/` 只有 iron_* 产物
- 唯一全周期检验过的 IronTrader 已证伪并保持 disabled:
  - 480 组合 sweep: futures 240/240 全负、spot 240/240 全负
  - 15m topcombo 3 周 +12.93, 其中 **79% 来自单笔**(largest_win 10.18)→ 拉长 3 个月 **−15.22**
  - 1h tuned 3 月 +21.28 = 8 笔交易、top1 单笔 +27.28(>100% 净利), 不可信
  - 5m 最差(3 周 −20.06): 止损变宽 + 费双杀
- **回测引擎不可认证"真钱生存力"**: 无资金费/保证金/强平/滑点, 成交 = 信号 K 收盘价即时
  (`docs/backtest.md:66-71` 自述边界); 且语义与实盘不一致(回测同向忽略+flip vs 实盘叠仓;
  实盘 `closed` 恒 true)→ **即便跑出好数字也不能外推**, 须先保真升级
- **存活候选只剩两个, 且都不够格**:
  1. **资金费率收割**: 7 年币安全史数据方向真实(独立复算 sign/0.5bp 模型 7 年 +68%/+56%,
     与记录 +61% 同量级; 5bp 事件闸仅 +24.8%; 费用 0.02%→0.04% 使 +68%→+48%; 2026 已约一年无窗口),
     但仓库口径三处不一、无复现脚本、**执行要件全缺**(FundingWatch 纯观察未部署; 无双腿配对执行、
     无现货↔合约划转、无空腿保证金/强平监控; 证据来自 Binance 而执行在 Gate)
  2. **网格**: 唯一有实盘 PnL 的自动策略, 但证据薄且为负(ETH v1 实盘 ≈ −33.6 USD; SNDK 全程 1 次成交);
     M27 引擎化版已实现却零含费统计
- **AI 调权**: 试点从未开跑(`docs/ab-weight-trial.md` §7 一票未投), 且归因断链污染输入 —— 归因修复前
  开 AI 心跳是负资产
- 工程 vs 研究: **工程缺口 100% 是"确定性修复"(周级); "能否赚钱"100% 由研究决定,
  最快也要周-月级日历时间, 且最坏结论是 NO-GO**

## 五、无人盯盘时最可能的结局

持仓裸奔(B1)→ 亏损静默累积(B2 熔断不存在、强平距离无监控)→ **强平是唯一兜底**, 事前事后都无告警(B3)。
重启时 halt/日亏状态清零(**fail-open**: 熔断被复位后自动恢复交易), 或 signal_only 下静默停摆
(**fail-silent**)—— 两个方向都是无声的。另: 手动 App 平仓习惯会叠加对账盲区(O7), 引擎敞口被高估、
幻影仓位占用预算, 后续"平仓"可能反手开新仓。

## 六、建议路径(按依赖排序)

### Phase 0 —— 今天, 零代码、零风险(30 分钟)

1. 上 Gate App/网页核对主网**余额/持仓/挂单** —— 若 09-27 停机时留有裸仓, 先人工了结
   (无兜底 SL, 这是当前第一风险)
2. 两处 `.env` → `chmod 600`; 核 key 权限(无关提现、配 IP 白名单, 必要时重建)
3. VPS 保持 inactive+disabled, 直到演练通过

### Phase 1 —— 修 8 个 blocker(约 1–2 周)

先摘低垂果实: B2 熔断接线(hours)、B7 幂等(hours); 再按天排 B1 止损双轨、B4 kill switch、
B5 私有 WS、B6 staleness/closed、B8 归因、B3 告警/看门狗。同时: 上线配置裁到 BTC 单品种、
per-instance auto_trade 持久化种子。

### Phase 2 —— 部署 + 演练(约 1 周)

重建 bundle(`c37364b` + 全部修复)→ 部署(仍 signal_only)→ **先落 logrotate 再长跑**(32 天写满的
倒计时是真的)→ systemd 看门狗 + 一条告警通道 → 三场演练: 断线重连(4 品种 + 私有回报)、
`kill -9` 崩溃恢复(halt 保持 / 孤儿挂单清扫 / 状态恢复)、告警逐事件注入。
**端到端演练(testnet 或主网 1 张最小单跑通"信号→下单→回报→对账")作为 go-live 闸门。**

### Phase 3 —— 验证(2–4 周, 双轨并行)

- **工程轨**: signal_only 纸面运行积累 → 单策略微资金 1–2 周(验收: 告警零漏报、零孤儿单、
  归因正确、止损/熔断实测触发)→ 再谈放大
- **研究轨**: 6 个 kline 策略含费样本外回测(门槛先写死: OOS 净为正 + 单笔盈利 <15% 净利 +
  参数高原稳定)→ 回测保真升级(funding/保证金/滑点/closed 语义)→ 纸面 ≥300 回合冻结契约裁决
- **平行轨**: 资金费率收割先落复现脚本、paper 捕获真实窗口, 再谈双腿 executor

**红线: 研究侧 GO 之前保持 `signal_only=true`。"能跑"与"有边际"是两个独立门槛, 禁止互相掩盖。**

## 附录: 验证记录(对抗性检查摘录)

- **safety 维度 19/19 条全部 confirmed**, 0 反驳
- **eng-ops 17 confirmed / 1 refuted**(AI_API_KEY 条, 见上文纠正)
- **edge 11 confirmed / 1 uncertain**("工程 60% / 研究 40%"的工作量占比属主观估计, 未纳入结论;
  "工程侧所有缺口都有确定修法"在其他维度复核后成立)
- 验证者补充的重要遗漏(已并入上文): 自动 CFD 入场同样无止损; 运行期调参不落盘; 聚合器残差锁死;
  trades.db 无灾备; 双引擎无 leader; 控制面零鉴权; 主循环 rest_mutex 阻塞面; 组合相关性共享预算;
  磁盘 32 天期限; 本地 1m 库实为 ~80% 覆盖(含整段空洞), 不可称"全量"
