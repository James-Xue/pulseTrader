# BTC 策略权重 A/B 纸面验证实验(M32)

> 状态:**试点未开始**(等待 VPS 部署 M32 信号日志后积累数据)
> 开始日期:引擎 `signals` 表首条记录落库之日
> 本文档 = 实验契约。§7 决策规则先行写死,实验结束只许执行、不许事后改。

## 0. 目标与假设

要回答的问题:**AI 每日调权(arm B)在含费净收益上是否显著优于等权基线(arm A)?**

- H0:AI 加权组合 ≤ 等权组合(净 PnL 无差异或更差)
- H1:AI 加权组合 > 等权组合

对照臂:

| 臂 | 权重策略 | 目的 |
|---|---|---|
| A 等权 | 全部 1.0(现引擎缺省) | 基线 |
| B AI 调权 | 每日 00:30(Asia/Shanghai)AI 提案 | 被测对象 |
| C AI 风控 | 等权 + AI 5 分钟巡检只记录 advisory(不执行) | 下轮实验备胎,本轮不启用 |

**配对结构是核心**:A/B 在同一份信号日志、同一 tape(ticker_ticks)、同一填充
模型上重放 → 日内差分剔除市场片段差异,小样本即有分辨力。

## 1. 数据基础

- `signals` 表(M32 引擎侧新表,append-only):每条策略原始信号
  (ts_ns/strategy_id/symbol/type/confidence/price/reason/indicators)+
  聚合共识行(strategy_id = `signal_aggregator`,重放时排除,留作日后
  实盘保真度核对)。**所有策略信号在 auto_trade 闸门之前落库** → 日志与
  权重策略无关,反事实重放的前提。
- 价格路径:`ticker_ticks`(record_market=true 时逐 tick 落库,futures
  kline 因 M30 不录,tape 足够)。
- 真实成交标定:`trades` 表(trades.db)。初值(2026-09-06 本机 409 行):
  SNDK futures maker 单 144 笔 / 成交率 72.9% / maker 腿滑点 0bps /
  market 腿 0.02–0.27bps。

## 2. 重放语义(tools/trial/paper_engine.py,镜像引擎)

- **聚合器**(SignalAggregator.cpp 逐行复刻):flat 忽略;per-symbol 累积
  buy/sell 加权置信与 weight_sum;normalized = dominant/ws < threshold(0.6)
  → 不发射;**cooldown(30s)闸先于发射,被闸时累积不清零继续增长**;
  发射后清零;缺省权重 1.0。注意引擎真实属性:**反向残留堆叠会长期闸死
  发射**(混合流归一化 < 0.6),方向反转需新方向 ≥ ~1.5× 旧残留 — 这是
  引擎语义,不是 bug。
- **账本**(PaperTrader 状态机):入场 maker-first 限价挂信号价,tape 回触
  (买:last ≤ P)即 maker 成交;超时(5s)→ taker 按当时 last 兜底;挂单期
  反向共识 → 撤单弃单;持仓期逐 tick 评估:TP 阶梯 0.5%/1%/2% × 1/3
  (taker)、Trailing SL(0.5% from best, taker)、max_hold 300s(taker)、
  反向共识平仓(taker);同向共识不 scale-in;入场费按平仓量比例分摊到各腿。
- 费用默认:maker 0.02% / taker 0.05%(Gate 合约档,2026-08 实测记忆;
  CLI 可覆写)。
- 单仓位、无 scale-in、退出价 = tape last(taker 腿)。**纸面 ≠ 实盘绝对
  收益**:填充模型有误差,但 A/B 同规则同 tape,误差权重无关,配对差分
  一阶消掉。

## 3. 试点周(第 1 周,信号率摸底)

- 记录:每策略每日信号数、聚合发射数、A/B 各自回合数、signals 表增速
- 功效:每臂 ≥ 300 回合(胜率标准误 ≈ 2.9%,可辨 ~5–6% 胜率差)
- 若发射率 ~20–60/天 → 主实验 14 个交易日,第 7 天中间检查点
- **若发射率过低(< 5/天)**:停下来重新评估(信号稀疏时先扩大策略集
  或调 threshold 才有统计意义 —— 此判定在试点周内做出,不算中途改规则)

## 4. AI Oracle 规格(每日一次)

- 触发 00:30 Asia/Shanghai;仅看前一日 23:59 前**已平仓**回合
- 输入(数值行):每策略 24h/72h 回合数/净 PnL/胜率/费用占比/均滑点 +
  regime 特征(1h/24h 已实现波动、EMA 快慢比、ATR%、价差 bps、资金费率)
  + 当前权重 + 最近 5 次自身调权记录
- 动作空间(harness 强制,AI 不可越界):输出
  `{"weights": {strategy_id: w, ...}, "rationale": "..."}`;
  约束 **w ∈ [0.05, 0.5]、单次变动 ≤ ±0.1、组内求和 = 1**
- 审计:每次决策落 `decisions` 表(旧→新权重、输入摘要 hash、模型、
  prompt 版本、原始响应),temperature 0、模型与 prompt 版本固定

**5 分钟巡检**:仅记录 advisory,不执行、不参与本实验统计。

## 5. 实验纪律(防混淆,先写死)

1. 参数冻结:实验期禁止 set_strategy_param(AI 调参是另一个变量)
2. 名单冻结:BTC 策略不增不减(新策略想法先在日志上回测,不进场)
3. 停机处理:引擎重启/断录日(>10% 1m 窗口 tick 缺失)从配对统计剔除,
   保留在报告中;systemd kill-9 自愈测试避开实验窗口
4. 永不丢失策略:日志在聚合之前记录,权重压到 0.05 地板不影响继续
   发信号 → 任何策略全程可复盘,防过拟合的结构性逃逸舱

## 6. 指标

| 维度 | 指标 |
|---|---|
| 主判据 | A/B 逐日**配对净 PnL 差**的均值与符号(配对 t + 符号检验) |
| 次判据 | 每回合期望(净)、各臂最大回撤、回合数、费用/净利比 |
| 稳定性 | 权重日间振荡量(天天大改 = 扣分,实盘同样会抖) |
| 敏感性 | 费用上下限(全 maker vs maker+taker)双跑,结论须一致 |

## 7. 决策规则(实验前写死)

主实验 14 交易日后:
- **GO(AI 权重接管实盘)**:配对日差均值 > 0 **且** 14 天中 ≥ 10 天为正
  **且** AI 臂总净利 > 0 — 三条件缺一不可
- **暂缓**:不满足 GO 但 AI 臂未明显更差 → 延长 14 天(仅一次),仍不达标
  → NO-GO
- **NO-GO**:AI 臂更差或打平 → 实盘保持等权,A 臂继续;AI 降级为纯风控
  咨询(即 C 臂角色)
- 无论结果:三臂净值曲线 + 权重轨迹 + 全部 AI 决策记录存档
  (data/trial/,git 不入库,由人工决策存档)

## 8. 工具与数据位置

| 物 | 位置 |
|---|---|
| 引擎信号日志 | VPS `~/pulseTrader/data/trades.db` `signals` 表(M32) |
| 重放核心 | `tools/trial/paper_engine.py`(+ `tests/test_paper_engine.py`,24 用例) |
| 重放 CLI | `tools/trial/run_replay.py` — equal/ai 两臂 |
| 标定 | `tools/trial/calibrate_fill_model.py` |
| 产物 | `data/trial/<symbol>/<arm>/{round_trips.csv, daily_summary.json}` |
| 引擎实现 | `src/trade_recorder/SignalRecorder.{hpp,cpp}`(config: `sqlite.record_signals`) |

运行示例:
```bash
# equal 臂(等权 1.0)
python3 tools/trial/run_replay.py --db data/trades.db --symbol BTC_USDT --arm equal
# AI 臂(权重文件归一化)
python3 tools/trial/run_replay.py --db data/trades.db --arm ai --weights-file w.json
# 标定
python3 tools/trial/calibrate_fill_model.py --db data/trades.db
```

## 9. 实施状态

- [x] M32 信号日志引擎实现 + 单测(5 用例),提交 1b4028d
- [x] 重放工具链 + 24 用例 + 标定初值,提交 a1dd5ac
- [ ] VPS 部署 M32(信号录制开始计时)— 进行中
- [ ] 试点周 → 主实验 → §7 裁决
