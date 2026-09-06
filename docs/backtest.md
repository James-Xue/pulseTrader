# Backtest — 通配回测框架 (M33)

在本地对 **任意 Gate USDT-M 期货币种** 做 1m K 线回测,复用与实盘完全相同的
C++ 策略代码路径。三个工具:

| 工具 | 作用 |
|---|---|
| `./run.sh backtest` | 单次回测:任意币种 × 任意注册策略 × 任意参数 |
| `./run.sh kline-store` | 数据积累:每日把任意币种的尾窗 K 线并入本地 sqlite |
| `./run.sh sweep` | 参数扫描:一次跑遍参数组合,CSV 汇总 |

## 数据模型(先读这段)

Gate REST futures 1m 端点**只保留最近 ~10000 根 ≈ 6.9 天**(实测;更早的
请求一律 400 "too long ago")。因此:

- **任意币种开测即用**:REST 现拉最近 ~7 天,无需任何配置。
- **~1 个月窗口的唯一来源是积累**:`kline-store` 每天拉一次尾窗落
  `kline_bars`(幂等 `INSERT OR IGNORE`),存储向后累积。**新币种约 30 天后
  才满 1 个月**;期间回测 = 已积累 + REST 尾部。
- 停机自愈:某次没跑没关系,下次拉尾窗自动补上(**中断 ≤ ~6.9 天无缝**);
  超 7 天的旧洞永远补不上(交易所不留),只能等未来数据。
- 引擎长期盯过的币种(BTC/ETH/SNDK/UNITREE 等)在 `data/trades.db` 已有
  数月积累,`kline-store --import` 一次导入即可跳启动(见下)。

## backtest — 单次回测

```
./run.sh backtest --strategy momentum_scalper --symbol DOGE_USDT \
    --from 2026-09-01 --to 2026-09-06 --quantity 20 [--json run.json]
```

注册策略:6 个,`--strategy` 任选。除 `orderbook_scalper` 外都吃 K 线
(orderbook_scalper 只吃实时盘口,回放下预期零信号,引擎会告警)。

### 关键 flag

| flag | 说明 |
|---|---|
| `--strategy NAME` | 注册键:`momentum_scalper` / `mean_reversion_scalper` / `supertrend_scalper` / `eth_scalper` / `ema_resonance_scalper` / `orderbook_scalper` |
| `--symbol PAIR` | 任意 USDT-M 合约,如 `DOGE_USDT`、`PEPE_USDT`;未知符号报 did-you-mean 错误(9306) |
| `--quanto Q` | 合约乘数(1 张 = Q 基础币)。**缺省自动解析**:公开合约表 → 缓存 12h。改缓存路径 `--contracts-cache P`,禁用 `--no-contract-cache`(离线且无缓存时明确报错要 `--quanto`,绝不静默按错误乘数算 PnL) |
| `--param KEY=VALUE` | **策略参数注入**(可重复,CLI 胜出 `--config`)。原子热参键:`order_quantity, min_confidence, ema_fast_period, ema_slow_period, bb_period, bb_std_dev, supertrend_period, supertrend_multiplier, cooldown_seconds, stop_loss_pct, take_profit_pct, auto_trade`;任何其它键走 custom_params 通道(`eth_atr_step`, `eth_spike_filter_*`, `res_ema_p1..p5`, ...)。JSON 报告的 `params.atomic / params.custom` 记录实际跑了什么 |
| `--quantity Q` | 开仓量(只做 PnL 缩放)。`--param order_quantity=` 只热更策略侧原子值,不改成交尺寸 |
| `--config PATH` | trading.toml 实例播种:quantity/min_confidence(仅当 CLI 未显式改过)+ **custom_params**(M33 起)。CLI 显式值胜出 |
| `--from/--to TIME` | 秒/毫秒 epoch 或 ISO UTC;缺省按本地覆盖自动解析 |
| `--no-api` / `--no-cache` | 只用本地存量 / 不写回 sqlite |
| `--db PATH` | kline_bars 库(默认 `data/trades.db`) |
| `--json PATH` | 全量 JSON 报告(数据来源、信号、交易、权益曲线、params) |
| `--fee-rate R` | <0 免手续费;0 市场默认(futures 0.0005 = 0.05% taker);>0 显式 |
| `--close-mode` | `flip`(默认,对向信号平旧开新)/ `independent`(多仓并存) |

### 惰性参数(注入了但 M33 成交模型不消费)

- `stop_loss_pct` / `take_profit_pct`:实盘风控用,M33 回测按 K 线收盘价即时
  成交,无 SL/TP 路径(intra-bar 保真模式是规划中的后续里程碑)。
- `ob_imbalance_threshold` / `ob_depth`:只被 orderbook_scalper 消费,kline
  回放下天然无效。

### 语义与限制(诚实的回测边界)

- 成交 = 信号 K 线**收盘价**即时成交;无滑点、无盘口、无保证金/强平/资金费率。
- 杠杆仅展示,不影响 PnL;收益公式与实盘 `PositionManager` 同源。
- 数据质量:sqlite 脏蜡烛(>25% 跳变等)自动净化,报告带警告。
- 本地库在旧 schema 下首次打开自动迁移 v2(PK 含 market_type),一次性几分钟。

## kline-store — 数据积累

```
./run.sh kline-store --symbols DOGE_USDT,PEPE_USDT          # 拉尾窗落库
./run.sh kline-store --config trading.toml                  # [backtest] 配置
./run.sh kline-store --import data/trades.db --symbols BTC_USDT,SNDK_USDT  # 导入存量
```

- 每日两次 cron(睡眠容错,幂等):

```
0 7,23 * * * cd /home/joey/1_Code/09_pulseTrader && ./run.sh kline-store --config trading.toml >> logs/kline_store.log 2>&1
```

  (systemd user timer 备选见 OPERATIONAL_GUIDE;公开 REST 无需 API key。)
- `[backtest]` TOML 段(可选):

```toml
[backtest]
store_db = "data/trades.db"
store_symbols = ["DOGE_USDT", "PEPE_USDT"]   # 缺省 = 配置里启用的 futures 策略实例符号
contract_cache_path = "data/contracts_cache.json"
```

- futures 符号先经合约表校验(typo 秒拒,不白拉 ~30 请求);spot 也可存
  (`--market spot`);CFD 不支持(REST 无时间窗端点,引擎自录是唯一来源)。
- `--import` 从任意 schema 版本的 kline_bars 库拷贝 futures 行(可带
  `--symbols` 过滤),为老盯盘币种一次补齐 ~1 个月。

## sweep — 参数扫描

```
./run.sh sweep --strategy momentum_scalper --symbol DOGE_USDT \
    --from 2026-09-01 --to 2026-09-06 \
    '--param' 'ema_fast_period=5|9|21' '--param' 'min_confidence=0.5|0.7' \
    --quantity 20 --workers 4 --out results/sweep_doge.csv
```

- 值语法:`5|9|21` 离散 / `34..100:11` 范围 / `2` 固定值,笛卡尔积组合;
  键经 `--param` 透传,atomic/custom 通道由引擎分类,与单跑语义一致。
- 其余 flag 原样透传给 backtest(见上表);`--max-combos` 默认 500 防爆。
- 首次 run 串行预热落库,并行 run 全走本地(CSV 的 `rows_api` 列应≈0)。
- 崩溃安全:每完成一行即重写 CSV;重跑同命令按签名跳过已完成组合
  (`--force` 全重跑)。失败行记 `exit_code` + stderr 尾部。
- **Sweep 参数集是实验产物,不是注册策略 → 不触发策略文档规则**;验证有效
  后若要实盘,请以配置/新 C++ 策略固化并补 `docs/strategies/` 文档。

## 快速上手(新币种第一天)

```bash
# 1. 积累从今天开始(cron 常驻后免手跑)
./run.sh kline-store --symbols DOGE_USDT

# 2. 立刻能测最近 ~7 天
./run.sh backtest --strategy momentum_scalper --symbol DOGE_USDT --quantity 20

# 3. 参数实验(本地数据预热后并行扫)
./run.sh sweep --strategy momentum_scalper --symbol DOGE_USDT \
    --from 2026-09-01 --to 2026-09-06 --no-api \
    '--param' 'ema_fast_period=5|9|21' '--param' 'ema_slow_period=34..50:8'
```

## 里程碑背景

M29 单策略 MVP(2026-08-23)→ M33 通配化:A1 合约元数据自动解析
(QuantoResolver)+ A2 全参数注入 → A3 kline-store 数据积累 → B sweep。
kline_bars schema v2(M0):PK 含 market_type,修复 spot/futures 同分钟行
互撞被静默丢弃的 bug(VPS daily sync futures 行曾长期被 spot 行挤掉)。
