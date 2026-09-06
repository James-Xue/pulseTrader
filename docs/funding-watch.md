# FundingWatch — 资金费率窗口监视器(引擎服务,2026-09-06)

> 纯观察服务:**只发信号板 + 日志,永不下单,不接聚合器**(旁路 auto_trade
> 与订单通道)。代码 `src/funding/FundingWatch.{hpp,cpp}`,注册段 `[funding_watch]`。

## 它解决什么问题

资金费率收割(现货多 + 合约空,收多头付空头的"情绪税")的正确形态不是
"天天收租",而是 **等费率窗口**(2021 牛市 BTC 持续 5-10bp/8h,平淡市
0.1-1bp)。7 年全史验证(`data/funding/`,2026-09-06):

| 年份 | BTC 净收(0.5bp 闸) | ETH 净收 | 市况 |
|---|---|---|---|
| 2020 | +16.3%/年 | +26.1% | 牛市 |
| 2021 | +29.1% | +36.4% | 大牛市 |
| 2022 | −1.6% | −4.3% | 熊市(闸后仍负) |
| 2024 | +9.9% | +12.0% | 牛市 |
| 2026 | ≈0 | ≈0 | 平淡(现价费率 0.1-1bp = 无窗口) |

→ 本服务的价值 = **把"等窗口"自动化**:窗口开启前引擎静默,开启瞬间
亮灯。将来接自动收割时,消费信号板 `funding_watch_<SYMBOL>` 行即可。

## 规则(语义故意做"笨")

1. **窗口 OPEN** = 最近 `consec_events`(默认 6 ≈ 48h)次结算的费率
   **全部严格大于** `threshold`(默认 0.0005 = 5bp/8h)。阈值不是 ≥:正好
   等于 = 噪声界,不开。
2. **窗口 CLOSED** = 任一次结算 ≤ threshold。无迟滞 —— 费率 8h 才结算
   一次,窗口不可能日内抖动。
3. 每次轮询(默认 30 分钟,费率 8h 动一次,足够)发布当前状态到信号板:
   - `strategy_id = funding_watch_<SYMBOL>`
   - type **Sell** = 窗口开启(语义:做空侧收租资格)、**Flat** = 关闭
   - indicators:`funding_latest` / `funding_avg_24h` / `window_threshold` /
     `window_events`;reason 带中文说明
4. 引擎日志:**仅窗口翻转时 WARN**(OPEN 一次 + CLOSED 一次),轮询静默
   —— 窗口开着就开着,不刷屏。

## 数据源与失败语义

- Gate 公开端点 `/futures/usdt/funding_rate`(8h 结算、保留 ~180 天),
  复用主 futures REST 客户端,持共享 `rest_mutex` 串行(引擎纪律)。
- 拉取失败/空响应 → 静默跳过本轮,错误日志节流(≥5 分钟一次);**绝不
  因网络抖动误报窗口**。窗口判定只看成功轮询的连续事件。
- 无自有线程:主循环 tick 槽(~200ms),内部慢闸限 poll_sec。

## 配置(`trading.toml.example` 同款)

```toml
[funding_watch]
enabled = true
symbols = ["BTC_USDT", "ETH_USDT"]   # futures 合约
threshold = 0.0005                    # 5bp/8h(牛市典型 5-10bp)
consec_events = 6                     # ≈48h 持续
poll_sec = 1800                       # 轮询间隔秒
```

校验(启用时):symbols 非空、threshold ∈ (0, 0.01]、consec ∈ [1,24]、
poll_sec ∈ [60, 86400]。

## 验收与测试

- [x] 单测(10,纯函数无网络):窗口判定(空/历史不足/持续高位开/单点缺口
      破坏/窗口外缺口无视/阈值严格大于/负费率)、Gate JSON 解析(顺序/
      截断/畸形跳过)—— 全绿(1042 全量,2026-09-06)
- [x] REST 形状实测(2026-09-06):`funding_rate?contract=BTC_USDT&limit=12`
      → `[{r: "0.000026", t: 1788681602}]` 倒序,与解析器一致
- [ ] 实盘验收(待 VPS 部署):开启后 `get_signals` 可见
      `funding_watch_BTC_USDT` 行 + 首次轮询日志;平淡市应常驻 Flat
- [ ] 窗口实拍(待牛市):连续 48h >5bp 时 WARN OPEN;回落 WARN CLOSED
