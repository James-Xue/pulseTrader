# SNDK 闪迪合约自动交易子代理 — 策略文档 v2

> **v1** (2026-08-18 网格初版,固定区间 1730-1800)→ **v2** (2026-08-19 复盘升级:动态滚动锚定 + ATR 步进 + 整体保护线)· 运行于 pulseTrader 引擎(mainnet 实盘)· 本文件是 SNDK 子代理的规则基线,修改前请先审阅
> 参照黄金代理 `xauusd-subagent-strategy.md` 的模式,但风控口径完全不同——**不要照抄黄金的数字**

## 1. 总览

在 pulseTrader 引擎上跑一个常驻后台子代理,对 SNDK_USDT(Gate.io 永续合约)执行**追空网格 v2**:

- **只做空**(用户授权;不做多,不逆势抢反弹)
- **动态滚动锚定**(v2 核心):网格区不固定,跟随行情移动。锚 A = round(盘口中间价 + 5, step),网格区 = [A, A + 20×step],价格远离网格时整体重挂(§3.1)
- **ATR 自适应步进**(v2):step = clamp(round(0.5 × ATR15m), 3, 8),初始 **5 点**;12 格(初始区间宽 60 点;**08-19 20:4x 用户上扩:20 格 / 区间 100 点**,见 §8)
- 每格 **2 张限价空单**(1 张 = 0.01 SNDK ≈ 16.7 USD 名义 @1670;2 张 ≈ 33 USD/格,网格总名义 ≈ **660 USD**)并随锚滚动
- **每格自带止盈**:成交后立即挂减仓限价买单(reduce-only),限价 = 该格成交价 - 10 点(2 个步进)。⚠️ Gate price_orders 触发单不支持部分平仓(1017/1014/1021/1038 实测),**严禁 place_trigger_order**,TP 一律走 `open_order` reduce-only 限价买单
- **单格不挂止损**(用户要求);**网格整体保护线**(v2 新增,§4)
- **循环重挂**:某格 TP 兑现且现价低于该格时,同一价格自动重新挂空单 + 新 TP(经典网格)
- 周期约 25~30 秒一轮:读实况 → 核对挂单 → 补挂/重挂 → 检查成交 → 挂 TP → 重锚检查 → 保护线检查 → 风控检查
- 每笔平仓写复盘 md 到 `0_note/`(`sndk-review-YYYYMMDD-HHMMSS.md`)

## 2. 市场与成本背景

| 项 | 值 |
|---|---|
| 交易方向 | SNDK_USDT 合约(`market_type=futures`,M22 后可在 cfd 活跃方向下执行手动单) |
| 合约规格 | 1 张 = 0.01 SNDK(quanto 0.01,风险门用真名义计算) |
| 行情 | 1m K 线 + ticker + order book(futures 全频道) |
| 手续费 | 合约 taker 0.05% / maker 0.02%(2 张 ≈ 33 USD 名义,单向 taker ≈ 0.017 USD) |
| 现状(08-19 20:52) | v2.1 网格:20 格限价空 1615~1710 步进 5,2 张/格;12 格(1615~1670)已成交持仓 + 1685~1710 六格 open(1675/1680 已成交);14 个 reduce-only TP 在列;mid ~1680,资金费 +0.0001 |
| 引擎策略 | SNDK 3 策略(momentum/mean_reversion/supertrend,signal_only)运行中 |

## 3. 网格操作协议(核心)

**每轮核对清单**(`list_futures_orders SNDK_USDT` = 交易所侧全部挂单;`list_trigger_orders` = 触发单;价格判断用盘口中间价 mid = (best_bid+best_ask)/2——⚠️ get_market 的 ticker.last 有缓存滞后 bug,禁用其判价):

1. **补挂缺失格**:20 格中某格无挂单(成交后未重挂/被撤)且 mid 尚未涨过该格 → 重新挂 `sndk-grid-<price>` 限价空 2 张
2. **成交 → 挂 TP**:某格成交(list_futures_orders 中 t-sndk-grid-* 消失且现价曾 ≥ 该格)→ 立即 `open_order`:symbol=SNDK_USDT, side=buy, quantity=2, market_type=futures, type=limit, price=成交价-10, **reduce_only=true**, client_order_id=sndk-grid-tp-<成交价>。⚠️ 严禁 place_trigger_order;严禁 reduce_only 缺失(会开反向仓)
3. **TP 兑现 → 重挂**:交易所侧 t-sndk-grid-tp-<成交价> 挂单消失(已成交)且 mid 低于该格 → 同一格价重新挂空单(循环)
4. **重锚检查**(§3.1,每轮执行)
5. **整体保护线检查**(§4,每轮执行)
6. **不干预**:用户的 App 挂单——只看不动

### 3.1 重锚协议(v2 动态滚动锚定)

- 锚 A = round(mid + 5, step),网格区 = [A, A + 20×step],只做空 → 网格区永远在现价上方等反弹
- **重锚触发(任一)**:
  - a) mid < 网格下沿 A - 15:价格远离网格下方(下跌趋势延续,网格闲置)→ 下移跟随
  - b) mid > 网格上沿 + 5:价格涨穿网格 → **不自动重锚**,转入保护线 A 检查(已成交格浮亏由保护线管)
- **冷却**:距上次重锚 ≥ 30 分钟(防抖,避免震荡市反复撤挂)
- **动作**:先撤全部自己的 `sndk-grid-*` 空单与 `sndk-grid-tp-*` 残留(仅自己前缀)→ 确认撤净 → 按新锚从下沿起挂 20 格(每格 `open_order`:sell, 2, futures, limit, price=<格价>, client_order_id=sndk-grid-<price>)
- 重锚后状态落盘,思考板记录新旧锚

## 4. 出场规则

> SNDK 期货**无交易所原生附件 SL/TP**(open_order 的 sl_price/tp_price 仅 CFD);止盈 = 每格独立的 **reduce-only 限价买单**(交易所侧常驻,引擎崩溃也生效,价格触及即只平该格 2 张)。price_orders 触发单仅支持**全平**,网格禁用。**单格无止损**(用户指定)。

| 规则 | 触发 | 动作 |
|---|---|---|
| 分格止盈 | 价格 ≤ 该格成交价-10 | 减仓限价买单自动只平该格 2 张 |
| **整体保护线 A**(v2) | 15m 收盘价 > 网格顶 + 20 点 | 推送报警 + 建议全平,**由用户决定是否平**(不自动平) |
| **整体保护线 B**(v2) | 网格浮亏累计 ≤ **-30 USD** | 推送报警 + 建议全平,**由用户决定是否平** |
| 日亏停手 | 当日已实现亏损累计 ≤ **-10 USD** | 当天不再开新仓/重挂(已有网格按原样管理) |

## 5. 风控与操作协议

- 每轮确认:`trading_halted=false`、限速 token 充足、余额够保证金
- 开仓/重挂:`open_order`(SNDK_USDT, sell, 2, market_type=futures, type=limit, price=<格价>)——**必须带 `client_order_id=sndk-grid-<price>`**
- 挂 TP:`open_order`(SNDK_USDT, buy, 2, futures, limit, price=成交价-10, reduce_only=true, client_order_id=sndk-grid-tp-<成交价>)——每轮核对 TP 挂单状态:若空仓且自己的 tp 挂单残留(用户全平导致),用 cancel_order 撤掉自己的 sndk-grid-tp-* 挂单(仅限自己前缀)
- **重锚**:先撤后挂(§3.1),冷却 30 分钟;撤单失败则本轮放弃重锚,下轮重试
- 平仓/撤单:一般不需要代理动手(TP 自动平);异常时 `close_position` 按张数平**自己该格的部分**(⚠️ 仓位合并,注意别平用户的部分)
- API 报错 → 跳过本轮,**绝不在错误状态下重试下单**
- 挂单被拒(如风控上限)→ 记录,不硬试;若 maxOpenPositions 报错 → 报告用户调配置
- 异常状态(余额突变、持仓不符、订单视图异常)→ 停止交易并记录
- 保守优先:**不确定就什么都不做**

## 6. 不碰的东西

- ❌ 用户的 SNDK 手动仓和全部 App 挂单——只在补挂自己的网格格位时动
- ❌ XAUUSD CFD、BTC_USDT、UNITREE_USDT、**ETH_USDT(含 eth-grid-* 前缀单,23:22 引擎重启后新增的他人网格)** 的一切 —— 其他代理和引擎策略的领地
- ❌ `halt_trading` / `switch_direction` / 策略参数修改 / `modify_sl_tp`
- ⚠️ `cancel_order`:只允许撤自己前缀的 sndk-grid-* / sndk-grid-tp-* 挂单;其他任何单不可撤
- ❌ 做多(追空授权仅覆盖空头方向)

## 7. 运行方式

- 后台子代理,每回合连跑 ~10 轮(约 4~5 分钟)后报告,主会话自动续接 → 不间断盯盘
- 与黄金/宇树代理**同时运行**:各自独立会话、独立文档、独立状态文件;共享引擎风控(全局熔断/日亏,一个触发全部停——这是保护)
- 用户可随时:喂思路/新闻(主会话转达)、喊停、调整规则
- 状态落盘:`0_note/gate交易/闪迪/joey-Z170I-PRO-GAMING/状态/sndk-agent-state.json`;交易复盘:`0_note/gate交易/闪迪/joey-Z170I-PRO-GAMING/复盘/sndk-review-YYYYMMDD-HHMMSS.md`
- 网格价格以 `list_futures_orders`(交易所侧真实挂单)为准,引擎 tracker 视图重启后会丢失旧单

## 8. 历史与变更

- **v1**(08-18):36 格 1730~1800 固定区间,步进 2,2 张/格。18h/2847 轮仅成交 1 次(1730×2),TP 从未实战触发——固定区间追不上单边行情,复盘见 `复盘/sndk-review-20260819-131500.md`
- **v2**(08-19):动态滚动锚定(§3.1)+ ATR 自适应步进(step=0.5×ATR15,3~8)+ 整体保护线(§4);格数 36→12,区间宽 60 点
- **v2.1**(08-19 20:4x):用户指令上扩网格范围——格数 12→**20**,区间 **1615~1710**,保护线 A = 顶+20 = **1730**;B 线/TP/重锚协议不变(任务书第 8 条)
- 平台教训(永久生效):price_orders 只支持全平(1017/1014/1021/1038)→ 引擎 M24 已加 size>0 拒绝;get_market ticker 缓存滞后 bug(未修,用盘口判价);tracker 视图重启丢单 → 一律交易所侧视图
- 首锚与参数经用户确认后生效,确认记录见任务书
