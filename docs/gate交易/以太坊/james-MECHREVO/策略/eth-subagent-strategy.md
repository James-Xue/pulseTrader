# ETH 以太坊合约自动交易子代理 — 策略文档 v2

> **v2** (2026-08-21,复盘 eth-review-20260821-grid-v1.md 后升级)· 运行于 pulseTrader 引擎(mainnet 实盘)· 本文件是 ETH 子代理的规则基线,修改前请先审阅
> v2 要点:**趋势闸门 + 暴拉检测 + 保护线 A 前置** —— 追空网格 v1 的致命伤是"上涨趋势中接飞刀 + 秒级穿顶暴冲无反应时间"(4 小时 3 次急拉,04:51 被用户 App 连带平仓 -39.2)
> 结构逐条镜像 `闪迪/joey-Z170I-PRO-GAMING/策略/sndk-subagent-strategy.md` **v2**(动态滚动锚定 + ATR 步进 + 整体保护线),价格/名义/费率按 ETH_USDT 实测替换——**风控口径不要照抄黄金的数字,也不要盲抄 SNDK 的绝对值,一切以本文档为准**

## 1. 总览

在 pulseTrader 引擎上跑一个常驻后台子代理,对 ETH_USDT(Gate.io 永续合约)执行**追空网格 v1**(= SNDK v2 结构):

- **只做空**(镜像 SNDK 授权口径;不做多,不逆势抢反弹)
- **趋势闸门(v2)**:只在 15m EMA 空头排列(EMA_fast < EMA_slow)时挂新格/重锚;多头排列期间**只管理存量**(TP 兑现、保护线),禁挂新格(§3.1c)
- **动态滚动锚定**:网格区不固定,跟随行情移动。锚 A = round(盘口中间价 + 1×step, step),网格区 = [A, A + 12×step],价格远离网格时整体重挂(§3.1)
- **ATR 自适应步进**:step = clamp(round(0.5 × ATR15m), 3, 8);12 格(初始区间宽 36 点)。step/tp_distance/grid_top 每次更新落盘到状态文件,机械 watchdog 从 state 读(eth_watch.py v2)
- 每格 **2 张限价空单**(1 张 = 0.01 ETH ≈ 21 USD 名义 @2097;2 张 ≈ 42 USD/格,网格总名义 ≈ 500 USD)
- **每格自带止盈**:成交后立即挂减仓限价买单(reduce-only),限价 = 该格成交价 - 2×step(step=3 时 -6)。⚠️ Gate price_orders 触发单不支持部分平仓(1017/1014/1021/1038 实测),**严禁 place_trigger_order**,TP 一律走 `open_order` reduce-only 限价买单
- **单格不挂止损**(镜像 SNDK 用户要求);**网格整体保护线**(§4)
- **循环重挂**:某格 TP 兑现且现价低于该格时,同一价格自动重新挂空单 + 新 TP(经典网格)
- 周期约 25~30 秒一轮:读实况 → 核对挂单 → 补挂/重挂 → 检查成交 → 挂 TP → 重锚检查 → 保护线检查 → 风控检查
- 每笔平仓写复盘 md 到 `0_note/gate交易/以太坊/james-MECHREVO/复盘/`(`eth-review-YYYYMMDD-HHMMSS.md`)

## 2. 市场与成本背景

| 项 | 值 |
|---|---|
| 交易方向 | ETH_USDT 合约(`market_type=futures`,M22 后可在 cfd 活跃方向下执行手动单) |
| 合约规格 | 1 张 = 0.01 ETH(quanto 0.01,风险门用真名义计算)——与 SNDK 的 quanto 相同,单张名义 ≈ 21 USD |
| 行情 | 1m K 线 + ticker + order book(futures 全频道) |
| 手续费 | 合约 taker 0.075% / **maker -0.01%(返佣)**——限价网格全程 maker,净收返佣(比 SNDK 更优) |
| 资金费 | -0.00013/8h(2026-08-20 实测)——**空头收取资金费**,对只做空网格有利 |
| 杠杆 | 合约最大 200 倍;网格用低杠杆(≤10)或默认,靠 12 格分散而非杠杆 |
| 现状(08-20 00:40) | mark 2097.6 / last 2094.97;ATR15m(1m TR 均值,40 根)≈ 2.87,ATR30m ≈ 3.34;order_size_min=0(2 张无最小量问题);价格精度 0.01 |
| 用户持仓 | ⚠️ **用户自己的 ETH_USDT 空单(App 管理,08-19 观察为 10 张 @1912)——纯观察不触碰** |
| 引擎策略 | ETH 3 策略(momentum/mean_reversion/supertrend,signal_only)**待配置**(见任务书),镜像 SNDK 的做法 |

## 3. 网格操作协议(核心)

**每轮核对清单**(`list_futures_orders ETH_USDT` = 交易所侧全部挂单;`list_trigger_orders` = 触发单;价格判断用盘口中间价 mid = (best_bid+best_ask)/2——⚠️ get_market 的 ticker.last 有缓存滞后 bug,禁用其判价):

1. **补挂缺失格**:12 格中某格无挂单(成交后未重挂/被撤)且 mid 尚未涨过该格 → 重新挂 `eth-grid-<price>` 限价空 2 张
2. **成交 → 挂 TP**:某格成交(list_futures_orders 中 t-eth-grid-* 消失且现价曾 ≥ 该格)→ 立即 `open_order`:symbol=ETH_USDT, side=buy, quantity=2, market_type=futures, type=limit, price=成交价-2×step, **reduce_only=true**, client_order_id=eth-grid-tp-<成交价>。⚠️ 严禁 place_trigger_order;严禁 reduce_only 缺失(会开反向仓)
3. **TP 兑现 → 重挂**:交易所侧 t-eth-grid-tp-<成交价> 挂单消失(已成交)且 mid 低于该格 → 同一格价重新挂空单(循环)
4. **重锚检查**(§3.1,每轮执行)
5. **整体保护线检查**(§4,每轮执行)
6. **不干预**:用户的 App 挂单和 ETH 手动仓——只看不动

### 3.1 重锚协议(动态滚动锚定)

- 锚 A = round(mid + 1×step, step),网格区 = [A, A + 12×step],只做空 → 网格区永远在现价上方等反弹
- **重锚触发(任一)**:
  - a) mid < 网格下沿 A - 3×step:价格远离网格下方(下跌趋势延续,网格闲置)→ 下移跟随
  - b) mid > 网格上沿 + 1×step:价格涨穿网格 → **不自动重锚**,转入保护线 A 检查(已成交格浮亏由保护线管)
  - **c) 趋势闸门(v2,前置条件)**:上述 a/b 触发后,**先查趋势再行动**——EMA_fast < EMA_slow(空头排列)才允许重锚/挂新格;多头排列 → 只管理存量(TP 兑现/保护线),**禁挂新格、禁重锚**,状态文件记 `trend_gate = "bullish"` 等待反转。追空网格 v1 教训:08-19~21 ETH 单边 +23% 上涨,网格全程逆势接飞刀。**快捷读法**:get_signals 的 `eth_scalper_ETH_USDT` 条目 indicators 恒带 `trend_state`(bullish/bearish/neutral)+ `spike`(0/1),新鲜度 ≤120s 时直接作闸门,不必自算 EMA
  - **d) 暴拉冻结(v2)**:state.grid.spike 存在(机械 watchdog 检测 1m 涨幅 > max(1%, 3×ATR15m) 落盘)→ 冻结新格挂单/重锚 30 分钟,存量 TP 照常管理;spike 清除后恢复
- **冷却**:距上次重锚 ≥ 30 分钟(防抖,避免震荡市反复撤挂)
- **动作**:先撤全部自己的 `eth-grid-*` 空单与 `eth-grid-tp-*` 残留(仅自己前缀)→ 确认撤净 → 按新锚从下沿起挂 12 格(每格 `open_order`:sell, 2, futures, limit, price=<格价>, client_order_id=eth-grid-<price>)
- 重锚后状态落盘(step/新锚/冷却),思考板记录新旧锚与触发原因

## 4. 出场规则

> ETH 期货**无交易所原生附件 SL/TP**(open_order 的 sl_price/tp_price 仅 CFD);止盈 = 每格独立的 **reduce-only 限价买单**(交易所侧常驻,引擎崩溃也生效,价格触及即只平该格 2 张)。price_orders 触发单仅支持**全平**,网格禁用。**单格无止损**(镜像 SNDK 用户口径)。

| 规则 | 触发 | 动作 |
|---|---|---|
| 分格止盈 | 价格 ≤ 该格成交价 - 2×step | 减仓限价买单自动只平该格 2 张 |
| **整体保护线 A(v2)** | **15m 收盘 或 1m 收盘** > 网格顶 + **2×step**(v1 为 4×step——04:50 暴冲 2124→2222 只用 2 分钟,4×step 限价判定无反应时间) | reduce_only 市价买入平掉网格自己份额(状态文件记录的已成交格数×2,严禁超份额)→ 复盘落盘 → 按 §3.1(含趋势闸门/暴拉冻结)决定是否重新锚定挂格。机械 watchdog 与 LLM 代理同规则 |
| **整体保护线 B** | 网格浮亏累计 ≤ **-30 USD** | 同保护线 A 的自主处置(平网格份额+落盘+重锚) |
| 日亏停手 | 当日已实现亏损累计 ≤ **-10 USD** | 当天不再开新仓/重挂(已有网格按原样管理);**口径(v2 拍板):北京日 08:00 重置**(与通宵任务书一致,00:00 夜盘不算新一天) |

> 网格份额口径:只统计 `eth-grid-*` 单成交的部分(每格 2 张),与用户手动仓无关;平仓用 reduce_only 市价买单,张数 = 网格已成交格数 × 2,一次不平超。

## 5. 风控与操作协议

- 每轮确认:`trading_halted=false`、限速 token 充足、余额够保证金
- 开仓/重挂:`open_order`(ETH_USDT, sell, 2, market_type=futures, type=limit, price=<格价>)——**必须带 `client_order_id=eth-grid-<price>`**
- 挂 TP:`open_order`(ETH_USDT, buy, 2, futures, limit, price=成交价-2×step, reduce_only=true, client_order_id=eth-grid-tp-<成交价>)——每轮核对 TP 挂单状态:若空仓且自己的 tp 挂单残留(用户全平导致),用 cancel_order 撤掉自己的 eth-grid-tp-* 挂单(仅限自己前缀)
- **重锚**:先撤后挂(§3.1),冷却 30 分钟;撤单失败则本轮放弃重锚,下轮重试
- 平仓/撤单:一般不需要代理动手(TP 自动平);异常时 `close_position` 按张数平**自己该格的部分**(⚠️ 仓位合并,注意别平用户的部分)
- API 报错 → 跳过本轮,**绝不在错误状态下重试下单**
- 挂单被拒(如风控上限)→ 记录,不硬试;若 maxOpenPositions 报错 → 报告用户调配置
- 异常状态(余额突变、持仓不符、订单视图异常)→ 停止交易并记录
- 保守优先:**不确定就什么都不做**

## 6. 不碰的东西

- ❌ **用户的 ETH_USDT 手动仓(App 管理,08-19 观察 10 张 @1912)和全部 App 挂单**——只在补挂自己的网格格位时动
- ❌ XAUUSD CFD、SNDK_USDT、UNITREE_USDT 的一切 —— 其他代理和引擎策略的领地
- ❌ `halt_trading` / `switch_direction` / 策略参数修改 / `modify_sl_tp`
- ⚠️ `cancel_order`:只允许撤自己前缀的 eth-grid-* / eth-grid-tp-* 挂单;其他任何单不可撤
- ❌ 做多(只做空口径,镜像 SNDK)

## 7. 运行方式

- 后台子代理,每回合连跑 ~10 轮(约 4~5 分钟)后报告,主会话自动续接 → 不间断盯盘
- 与黄金/闪迪/宇树代理**同时运行**:各自独立会话、独立文档、独立状态文件;共享引擎风控(全局熔断/日亏,一个触发全部停——这是保护)
- 用户可随时:喂思路/新闻(主会话转达)、喊停、调整规则
- 状态落盘:`0_note/gate交易/以太坊/james-MECHREVO/状态/eth-agent-state.json`;交易复盘:`0_note/gate交易/以太坊/james-MECHREVO/复盘/eth-review-YYYYMMDD-HHMMSS.md`
- 网格价格以 `list_futures_orders`(交易所侧真实挂单)为准,引擎 tracker 视图重启后会丢失旧单
- ⚠️ 部署前核验:本机引擎二进制是否含 M23/M24 工具(`list_futures_orders`/`list_trigger_orders`);若缺失,以 `get_orders` 交易所侧视图兜底,并报告主会话

## 8. 参数对照与历史

### 8.1 SNDK v2 → ETH v1 参数对照(镜像关系)

| 参数 | SNDK v2(已拍板) | ETH v2(已拍板) | 等价关系 |
|---|---|---|---|
| step | 5 | **ATR 自适应 clamp(round(0.5×ATR15m), 3, 8)** | 正式落地(v1 实际用了固定 5);深夜 ATR 3.44 → step 3 |
| 格数 | 12 | **12** | 相同 |
| 每格张数 | 2(≈33 USD) | **2**(≈42 USD) | 相同张数;ETH 单张名义更高 |
| 锚偏移 | mid + 5 | mid + 1×step | 1×step |
| 分格止盈 | 成交价 - 10 | 成交价 - 2×step | 2×step |
| 重锚下移阈值 | 下沿 - 15 | 下沿 - 3×step | 3×step |
| **趋势闸门(v2 新增)** | 无 | **15m EMA 空头排列才挂新格/重锚;多头排列只管理存量** | 复盘教训:上涨趋势中追空=接飞刀 |
| **暴拉冻结(v2 新增)** | 无 | **state.grid.spike(1m 涨幅>max(1%,3×ATR15m))→ 冻结新格 30 分钟** | 复盘教训:04:51 穿顶暴冲 2 分钟 +98 点 |
| 保护线 A | 顶 + 20 | 顶 + **2×step**,**15m 或 1m 收盘超线即触发** | v1 4×step 无反应时间 |
| 保护线 B | 浮亏 ≤ -30 USD | 浮亏 ≤ **-30 USD** | 相同 |
| 日亏停手 | ≤ -10 USD | ≤ **-10 USD,北京日 08:00 重置** | 口径拍板 |
| 重锚冷却 | 30 分钟 | 30 分钟 | 相同 |
| 方向 | 只做空 | 只做空 | 相同 |

### 8.2 尺度差异提示(拍板前请过目)

- ETH 价格尺度(≈2097)是 SNDK(≈1612)的 1.3 倍,但 1m 波动(ATR15m 2.87)远小于 SNDK(约 10):同公式下 **step 3 仅占价格 0.14%**,而 SNDK 的 step 5 占 0.31%。若希望「相对波动感」对齐 SNDK,可将 step 下限放宽(如 clamp 6~16 → 当前算得 ~7);**保持公式不变 = 绝对步进偏紧、成交更频繁但单格利润更薄**。两种口径二选一,由用户拍板。
- 网格名义 ≈ 500 USD 全量成交,占引擎 maxSymbolNotional(5500)约 9%,与 SNDK 网格(≈400 USD)量级一致;12 格全成交 = 12 个独立仓位,部署前需复核 `maxOpenPositions`(SNDK 当时 4→40,现配置为 4)。
- 资金费 -0.00013(空头收)+ maker 返佣:持仓期成本为负,对网格持有有利。

### 8.3 历史与变更

- **v2**(08-21,复盘后拍板):趋势闸门(15m EMA 空头排列才挂格)+ 暴拉冻结(state.grid.spike)+ 保护线 A 前置(2×step + 1m 触发)+ step ATR 自适应正式落地 + 日亏北京日 08:00 重置;引擎 9103 预算已修(12000/6500)、eth_scalper 信号源已启用(signal_only)
- **v1**(08-20):镜像 SNDK v2 创建;实盘 08-19 23:16 部署 → 04:51 被用户 App 连带平仓 -39.2(复盘 `复盘/eth-review-20260821-grid-v1.md`)
- **镜像来源 SNDK 教训**(永久生效):v1 固定区间 18h/2847 轮仅 1 成交 → v2 改动态滚动锚定;price_orders 只支持全平(1017/1014/1021/1038)→ 严禁 place_trigger_order;get_market ticker 缓存滞后 bug(用盘口判价);tracker 视图重启丢单 → 一律交易所侧视图
