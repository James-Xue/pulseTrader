# M26 策略架构:UnifiedScalper + Registry 兜底 + 币种策略 (2026-08-21, ff616b6/8fb4021, 817 绿)

> `project-memory.md`「M26 策略架构」一节的细节展开。

## UnifiedScalper(`src/strategy/scalping/`)

kline 驱动策略的模板方法基类——onKline/onTick final,computeAtr/warmup/cooldown/no-data 日志从 3 处逐字重复收敛为一处;momentum/mean_reversion/supertrend 已迁移继承,**行为逐字节等价**(strategy_id/日志文案/cooldown 语义——Momentum 显式禁用/ATR 归一化置信度均不变);orderbook_scalper 保持独立(数据源不同)

## StrategyRegistry(`src/strategy/StrategyRegistry.{hpp,cpp}`)

- TOML `name` = 注册键,`makeBuiltinStrategyRegistry()` 集中注册
- **未注册名 → 被动 UnifiedScalper(默认 evaluateEntry→nullopt,永不发信号)+ WARN 列已注册名**(不再 warn+skip,引擎不会因拼错配置退出)
- 重名注册拒绝;替代 main.cpp 硬编码 if-chain 工厂

## custom_params

- 实例级 TOML 内联表 `custom_params = { key = value }`(array-of-tables 下唯一合法子表形式)→ `StrategyInstanceConfig.custom_params` map
- 严格类型校验(非 table/非数字报 ConfigInvalidValue,缺键空 map 向后兼容)
- `UnifiedScalper::customParam(key, fallback)` 静态读取,无热更新

## EthScalper(注册键 `"eth_scalper"`)

首个币种策略示范——追空 EMA bearish cross(只做空)+ 暴拉过滤 + ATR 自适应止盈(`eth_atr_step` 默认 0.05,suggested_tp = close - step×atr)+ 置信度缩放(`eth_min_confidence_scale`)

## EthScalper v2(08-21,M26.1,ETH 网格复盘后升级,819 绿)

① 每 candle 发布 Flat+conf=0 **状态信号**,indicators 恒带 `trend_state`(bullish/bearish/neutral)+ `spike`(0/1)——信号板永远有最新趋势状态,网格子代理读 get_signals 即得挂格闸门(不需自算 EMA)
② 暴拉过滤**三口径**:`eth_spike_filter_usd`(120)+ `eth_spike_filter_pct`(1.5%)+ `eth_spike_filter_atr`(3×ATR),任一触发即过滤,设 0 禁用(复盘教训:USD 单口径挡不住 04:50 1m +4.4% 暴拉)
③ 真信号语义不变(bearish cross 且非 spike 才 Sell)

## futures 幽灵仓剪枝(08-21,M26.1)

syncFuturesPositionsFromExchange 收集 live_contracts,`pruneGhostFuturesByContract` 剪掉交易所已无持仓合约上的 fill-tracked 仓(60s 宽限;同步失败绝不剪)。背景:08-19 ETH 网格 15 个 eth-grid-* 仓被用户 App 04:51 全平后,因 exchange_position_id 为空永久残留 + 假 upnl +623

## 加新币种策略套路

继承 UnifiedScalper 覆写钩子(className/idPrefix/klineNeeded/warmupThreshold/cooldownEnabled/evaluateEntry/buildSignal/logSignal)→ StrategyRegistry.cpp 注册一行 → src/strategy + tests/unit/strategy 两个 CMakeLists 各一行 → FeedHarness kline 全链路测试(模式见 tests/unit/market/test_market_feed_sink.cpp:55-71,MarketFeed 构造无 I/O,getKlineBuffer(symbol).push 注入蜡烛)

## 已知

clangd 对 PULSE_LOG_INFO 格式串报 invalid_consteval_call 是 LSP 误报(新文件未入编译数据库),真实编译零警告
