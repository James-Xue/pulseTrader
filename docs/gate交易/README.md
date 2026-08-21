# gate交易 策略文档目录(commit_my_life 镜像)

> 本目录是 `~/1_Code/commit_my_life/0_note/gate交易/` 的**结构镜像 + 权威文档快照**。
> 组织方式:市场 × 机器 × 类别(任务书 / 策略 / 思考板 / 状态 / 复盘 / 统计 / 工具)。
>
> ⚠️ **改动请回源**——本目录是快照,commit_my_life 才是策略文档的权威源;
> 引擎化(策略 → C++ 代码)的进度见 [以太坊/eth-grid-cpp-migration.md](以太坊/eth-grid-cpp-migration.md) 与 `src/grid/`。

## 目录结构

```
docs/gate交易/
├── Gate期货触发单TP-SL管理技术备忘_2026-08-16.md   # 根级共享备忘(price_orders 三接口)
├── 黄金/      # XAUUSD CFD 剥头皮 + SignalBoard 因子板
│   └── joey-Z170I-PRO-GAMING/{任务书,策略}         # 主运营机
├── 闪迪/      # SNDK_USDT 永续追空网格
│   ├── james-MECHREVO/任务书                        # v2.5 接管版
│   └── joey-Z170I-PRO-GAMING/{复盘,策略}            # v2.1 基线
└── 以太坊/    # ETH_USDT 永续追空网格(v2 = GridManager C++ 化的规格书)
    ├── james-MECHREVO/{策略,任务书}                 # v2 规范版所在地
    └── joey-Z170I-PRO-GAMING/复盘                   # v1 全周期复盘(教训→C++ 化输入)
```

空机器目录(ZhangdeMacBook-Pro 等)以 `.gitkeep` 占位,保持与源结构一致。
`trading.toml` 中引擎策略实例与 [grid] 段是**本仓库内**的另一类配置,不在此目录。

## 权威文档速查表

| 市场 | 文档 | 用途 | C++ 化状态 |
|------|------|------|-----------|
| 以太坊 | [策略/eth-subagent-strategy.md](以太坊/james-MECHREVO/策略/eth-subagent-strategy.md) | **ETH 网格 v2 规范**(趋势闸门/暴拉冻结/ATR 自适应/保护线 A 前置) | ✅ 已引擎化 → GridManager(`src/grid/`) |
| 以太坊 | [任务书/eth-agent-taskbook.md](以太坊/james-MECHREVO/任务书/eth-agent-taskbook.md) | v1.1 接管任务书(参数基线) | ✅ 参数并入 [grid] TOML |
| 以太坊 | [复盘/eth-review-20260821-grid-v1.md](以太坊/joey-Z170I-PRO-GAMING/复盘/eth-review-20260821-grid-v1.md) | v1 全周期复盘:缺趋势闸门/9103 名义/幽灵仓等教训 | ✅ 教训全部落入 v2 规则 |
| 闪迪 | [策略/sndk-subagent-strategy.md](闪迪/joey-Z170I-PRO-GAMING/策略/sndk-subagent-strategy.md) | 追空网格 v2.1 基线(ETH v1 的镜像来源) | ⏳ 未引擎化(维持子代理运行) |
| 闪迪 | [任务书/sndk-agent-taskbook.md](闪迪/james-MECHREVO/任务书/sndk-agent-taskbook.md) | 家庭机 v2.5 接管版 | ⏳ 同上 |
| 闪迪 | [复盘/sndk-review-20260819-155300.md](闪迪/joey-Z170I-PRO-GAMING/复盘/sndk-review-20260819-155300.md) | v1 固定区间 → v2 动态锚定转折复盘 | ⏳ 教训已入 SNDK v2 规则 |
| 黄金 | [策略/xauusd-signal-board-design.md](黄金/joey-Z170I-PRO-GAMING/策略/xauusd-signal-board-design.md) | SignalBoard 因子板设计规格 | ✅ 已引擎化(M20,`src/strategy/signal/`) |
| 黄金 | [策略/xauusd-subagent-strategy.md](黄金/joey-Z170I-PRO-GAMING/策略/xauusd-subagent-strategy.md) | XAUUSD CFD 剥头皮规则基线 | ⏳ 未引擎化(子代理运行) |
| 黄金 | [策略/xauusd-strategy-changelog.md](黄金/joey-Z170I-PRO-GAMING/策略/xauusd-strategy-changelog.md) | 策略版本史 | — |
| 黄金 | [任务书/xauusd-agent-taskbook.md](黄金/joey-Z170I-PRO-GAMING/任务书/xauusd-agent-taskbook.md) | 黄金子代理任务书 | ⏳ 未引擎化 |

## 不拷贝内容(仍在 commit_my_life 源目录)

- **状态 JSON**(`*-agent-state.json`)— 运行产物,机器侧实时状态
- **工具脚本**(`gate_ledger.py` / `sndk_grid.py` / `eth_watch.py` / `relay_*.sh` 等)— 含机器绝对路径,属运行环境;引擎化后逐步退役
- **思考板**(`*-agent-thinking.md`)— 回合流水,非权威规则
- **复盘流水**(黄金 20+ 篇)— 只镜像有转折意义的关键复盘
- **统计 / 会话通信 / 锁与日志 / __pycache__**

## 引擎化指引

- 本目录文档是"文字策略";`src/grid/` 是 ETH 网格 v2 的 C++ 实现
- 逐条规则映射见 [以太坊/eth-grid-cpp-migration.md](以太坊/eth-grid-cpp-migration.md)
- 网格控制面:`grid_start / grid_status / grid_pause / grid_resume / grid_stop`(REPL/JSON-RPC/MCP 三通道)
