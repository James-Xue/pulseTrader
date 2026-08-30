# AGENTS.md — AI Coding Assistant Guidelines

> This file tells AI coding assistants (Claude Code, GitHub Copilot, Cursor, etc.)\
> how to work effectively within the pulseTrader codebase.

---

## Project in 30 Seconds

**pulseTrader** is a C++20 high-frequency scalping framework targeting Gate.io exclusively. It integrates real-time market data with AI-driven sentiment analysis (LLM every 5 minutes) to adaptively tune strategy parameters. Architecture is 9 vertical layers — read `docs/architecture.md` before making any structural changes.

```
L1 Exchange → L3 Market Data → L6 Strategy → L7 Risk → L8 Execution
L5 Heartbeat → L4 AI → ParamAdvisor (atomic writes to L6)
L2 Logging (cross-cutting) · L9 Control Plane (JSON-RPC socket + CLI REPL + MCP, cross-cutting)
```

**Key property**: the market data hot path (L1→L3→L6→L7→L8) must never block on AI inference, disk I/O, or external network calls. The AI pipeline runs on an isolated background thread (L5 heartbeat worker) and communicates with L6 via lock-free `std::atomic` parameter writes.

---

## Build & Run

```bash
# Configure with vcpkg (first time)
cmake -B build -S . -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake

# Configure without vcpkg (Linux: apt + vendored websocketpp)
cmake -B build -S . -DCMAKE_BUILD_TYPE=Debug \
      -DPULSE_ENABLE_SQLITE=ON

# Build
cmake --build build -j$(nproc)

# Run tests (902 tests)
ctest --test-dir build --output-on-failure

# Optional features
cmake -B build -S . -DPULSE_ENABLE_SQLITE=ON
```

Dependencies are managed by **vcpkg** (preferred) or **apt + vendored `third_party/`** (Linux alternative). On Linux, websocketpp is vendored in `third_party/`; do NOT add it to `vcpkg.json` if building without vcpkg.

## Run Modes

`./run.sh` supports `{trade|cli|mcp|rest|ws|market|strategy|ai|test}` (the `webui` mode was removed with the WebUI):

- `./run.sh trade` — trading engine (default subcommand `trade`); embeds a REPL when stdin is a TTY, and opens the JSON-RPC control socket (TCP 127.0.0.1:8081, `[control]` TOML section, `PULSE_CONTROL_PORT` env override)
- `./run.sh cli` — remote-attach REPL over the control socket (auto-loads `trading.toml`; the engine must be running)
- `./run.sh mcp` — stdio MCP server bridging to the control socket (auto-loads `trading.toml`), for LLM clients like Claude Desktop / Claude Code

**Production deployment**: the engine runs as the systemd user service `pulsetrader.service` (`~/.config/systemd/user/`, `loginctl enable-linger`) — auto-start at boot, `Restart=on-failure`, journald logs (`journalctl --user -u pulsetrader -f`). Restart after rebuilding: `systemctl --user restart pulsetrader`.

**Single instance**: `runTrade()` takes an exclusive `flock` on `data/engine.lock` and exits if another engine holds it. NEVER start a second engine (manual `./run.sh trade`, another session, a nohup) while one is running — two engines trade independently and the engine view drifts from the exchange position (2026-08-16 incident). `PULSE_ALLOW_MULTI_INSTANCES=1` bypasses the lock for deliberate multi-instance setups only. `cli`/`mcp` subcommands must NOT take the lock.

## Control Plane Rule

The control socket speaks newline-delimited JSON-RPC 2.0 over TCP. **Method names on the control socket ARE the MCP tool names** — keep them identical when adding a method. The 32 methods/tools are: `get_status`, `get_account`, `get_positions`, `get_orders`, `list_strategies`, `get_strategy_params`, `set_strategy_param`, `set_strategy_trading`, `open_order`, `close_position`, `cancel_order`, `halt_trading`, `resume_trading`, `get_risk`, `get_market`, `pause_strategy`, `resume_strategy`, `switch_direction`, `get_signals`, `sync_positions`, `modify_sl_tp`, `get_param_history`, `get_strategy_performance`, `grid_start`, `grid_status`, `grid_pause`, `grid_resume`, `grid_stop`, `place_trigger_order`, `list_trigger_orders`, `list_futures_orders`, `cancel_trigger_order`. REPL commands in `CommandParser` map to these methods. Manual orders and the signal aggregator both flow through `OrderFlowExecutor` (Layer 8). **Per-strategy execution gate**: each strategy carries a runtime `auto_trade` flag (StrategyParams, audited); signals from disabled strategies still publish to the `SignalBoard` but never enter the aggregator — the gate lives in the strategy→aggregator wiring in `main.cpp`, NOT in `OrderFlowExecutor::onSignal` (the aggregator output loses the strategy id). `[strategy] signal_only = true` seeds every strategy's `auto_trade` to OFF at startup (fail-safe on restart); flip a single strategy at runtime via `set_strategy_trading` / REPL `autotrade <id> on|off`. The gate is independent of the active-market direction gate and of manual orders (`open_order` — the sub-agent's path — stays live). `get_signals` reads the `SignalBoard` (strategy signals + indicator snapshots + aggregator consensus). `sync_positions` reconciles the engine position view against the exchange (futures + CFD) and prunes local ghosts from manual app-side closes (runs at startup, every ~10s, and on demand). `modify_sl_tp` dynamically adjusts an open CFD position's exchange-native price_sl/price_tp (CFD only; 0 clears a stop).

---

## Rule #1 — English Only

**All documentation, code comments, log messages, commit messages, and variable names must be written in English.** This is an open-source project; the audience is global. No Chinese in source files, headers, docs, or git history.

---

## Coding Conventions

| Rule | Detail |
|------|--------|
| **Language** | C++20. Use `std::jthread`, `std::stop_token`, `std::ranges`, concepts, `std::atomic<double>`. |
| **Namespace** | All code in `pulse::` or sub-namespaces (`pulse::exchange`, `pulse::market`, etc.). |
| **Headers** | `#pragma once` (not `#ifndef` guards). |
| **File naming** | **Filename must match the primary class name**: `OrderExecutor.hpp` / `OrderExecutor.cpp` for `class OrderExecutor`. Multi-type modules (e.g., `config.hpp`, `types.hpp`, `risk_types.hpp`) keep descriptive `snake_case` names. |
| **Include style** | `"pulse/layer/module.hpp"` for project headers, `<nlohmann/json.hpp>` for third-party. |
| **Error handling** | Use `pulse::PulseError` exception hierarchy for fatal errors; return `std::expected<T, PulseError>` (C++23) or `std::optional` + log for non-fatal. Never swallow errors silently. |
| **Logging** | Use `PULSE_LOG_INFO/WARN/ERROR(module, fmt, ...)` macros — never `std::cout`. |
| **Thread safety** | Hot path data structures: prefer lock-free (atomics, seqlock). Only use `std::mutex` when lock-free is impractical. Document thread-safety in header comments. |
| **Naming** | Classes: `PascalCase` (no underscores). Functions/methods: `camelCase` (no underscores). Member variables: `m_camelCase` prefix. Constants: `kPascalCase`. Private members use `m_` prefix, NOT trailing underscore. Struct data fields (pure-data containers like `OrderRequest`, `Position`, config structs) keep `snake_case`. |
| **Braces** | **Always use braces** for `if`, `else`, `for`, `while`, `do-while` — even for single-line bodies. No `if (x) return;` on one line. |
| **Yoda conditions** | Put the constant/literal on the **left** side of comparisons: `if (0 == status)` not `if (status == 0)`. Prevents accidental `=` assignment. |
| **Comments** | **Required and detailed.** Use numbered lists for multi-step logic. See Comment Style below. |

### Comment Style

Every non-trivial function, class, and block must have detailed comments explaining intent, not just mechanics. For multi-step logic, use a **numbered list with line breaks**:

```cpp
// Process incoming tick and update internal state:
// 1. Validate timestamp is within acceptable skew window
// 2. Update best bid/ask in the lock-free order book snapshot
// 3. Recalculate mid-price and spread
// 4. If spread exceeds threshold, flag as illiquid and skip strategy evaluation
// 5. Publish updated snapshot to downstream consumers via atomic store
void MarketDataProcessor::on_tick(const TickEvent& tick) {
    // ...
}
```

**Where to comment**:
- Every public function/method: document purpose, parameters, return value, and thread-safety guarantees
- Every class: document ownership, lifetime, and which layer it belongs to
- Every `if`/`switch` with more than 2 branches: explain the decision tree
- Every lock-free or atomic operation: explain the memory ordering choice
- Every error path: explain what failed and why we recover (or don't)

**Never** write comments that just restate the code (`// increment counter`). Explain **why**, not **what**.

---

## Layer Boundaries

Each layer communicates only with adjacent layers through narrow typed interfaces. **Never** bypass this:

- ✅ Strategy (L6) reads from Market Data (L3) via `TickerCache::load()` / `OrderBookManager::snapshot()`
- ❌ Strategy (L6) must NOT call `GateWsClient` directly
- ✅ AI (L4) writes to Strategy params via `ParamAdvisor::apply(AnalysisResult)`
- ❌ AI (L4) must NOT mutate `StrategyParams` fields directly

When adding a new capability, ask: **which layer owns this responsibility, and which interface should it expose?**

---

## Testing

- **Unit tests**: `tests/unit/test_<module>.cpp` — use GTest, no network I/O, run in `ctest`.
- **Integration tests**: `tests/integration/test_<flow>.cpp` — may hit Gate.io testnet or a mock server.
- **Smoke tools**: `tools/test_<feature>.cpp` — manual verification scripts, not in CTest, print to stdout.

All new code needs unit tests. Integration tests are required for any cross-layer flow.

---

## Things to Avoid

1. **Don't add Boost.** The project uses standalone `asio`, not `boost::asio`. `websocketpp` was chosen over `crow`/`boost::beast` specifically to avoid a Boost dependency. (The WebUI's uWebSockets/uSockets were removed on the `headless` branch; only `third_party/websocketpp` remains vendored.)
2. **Don't add new exchange abstractions.** pulseTrader targets Gate.io only — depth of integration over breadth.
3. **Don't put blocking I/O on the market data thread.** AI calls, file writes, and REST requests go on dedicated background threads.
4. **Don't parse LLM responses as free text.** AI output must conform to the fixed JSON schema in `analysis_result.hpp`. Validation failure → discard, keep old params.
5. **Don't add dependencies to `vcpkg.json` without checking the optional-feature flag.** New HTTP libraries, JSON parsers, etc. should reuse existing deps. Also note: Linux builds use apt + vendored `third_party/` — CMakeLists.txt changes for vcpkg targets will break Linux builds.

---

## Documentation

- `docs/architecture.md` — full 9-layer architecture, module reference, data flow, threading model
- `docs/highLevelArchitecture.md` — condensed visual overview
- `docs/howItWorks.md` — narrative walkthrough
- `docs/implementation-roadmap.md` — phased build order (L2→L1→L3→L8→L7→L6→L5+L4→L9)
- `src/control/` — Layer 9 control plane: `JsonRpcServer` (TCP socket), `CommandParser` (REPL), `McpServer` (stdio MCP), `ControlClient`, `EngineServices`, `OrderFlowExecutor`

When making architectural changes, update `architecture.md` to keep it as the source of truth.

---

## Git

- Commit messages: conventional style (`feat:`, `fix:`, `refactor:`, `docs:`, `test:`, `chore:`).
- Keep commits atomic — one logical change per commit.
- Don't commit API keys, secrets, or `.env` files. The `.gitignore` already covers `.claude/` and build artifacts.
- **Pushing is manual-only** (user rule, 2026-08-30): never run `git push` automatically — not after commits, not as part of any workflow. Leave the push to the user; they push manually.

---

## When in Doubt

Ask before assuming. The architecture document is the source of truth; if it's silent on a topic, check `docs/howItWorks.md` and `docs/implementation-roadmap.md` before guessing.


# 仓库工作说明
本文件用于指导 Codex 一类编码代理在本仓库中的分析、修改、注释增强与验证行为。

## 1. 适用范围
- 本文件位于仓库根目录，作用域覆盖整个仓库。
- 若更深层目录存在额外的 `AGENTS.md`，则更深层文件优先。

## 2. 仓库概览
- 本仓库是大型 `C++/Qt + Visual Studio + CMake` 工程。
- 根构建入口为 `CMakeLists.txt`。
- 主要代码集中在 `src/`。
- 机械相关核心模块集中在 `src/Mech/`。
- 平台与工作台模块集中在 `src/`。

## 3. 工作原则
- 优先做小而准的修改，不做无关重构。
- 优先修复根因，不做表面绕过。
- 后续排查、对比与提交时，默认优先关注 `src/` 目录下的代码变动；除非用户明确要求，否则不把根目录其他区域的零散变动作为主要提交范围。
- 未经用户明确要求，不要修改与当前任务无关的文件。
- 未经用户明确要求，不要删除文件、批量重命名、移动目录或重置已有改动。
- 尽量保持现有代码风格、目录结构、命名方式和缩进习惯。

### 3.5 比较表达式常量左置（Yoda Condition）
- 在 `if` / `while` / `switch` 等条件判断中，常量（字面量、`nullptr`、`true`、`false`、枚举值、宏常量）必须放在比较运算符**左侧**，变量放在右侧。
- 正确：`if (nullptr == pTracker)`、`if (0 == nCount)`、`if (TRUE == bFlag)`、`if (IDOK == nResult)`
- 错误：`if (pTracker == nullptr)`、`if (nCount == 0)`、`if (bFlag == TRUE)`、`if (nResult == IDOK)`
- 目的：防止将 `==` 误写为 `=`（赋值），编译器会在常量被赋值时报错。
- 新增或修改比较表达式时必须遵循此规则；存量代码如未涉及则不主动批量修改。

### 3.6 卫语句（Guard Clause）
- 在函数开头处理边界条件、异常情况或提前返回，避免深层嵌套的 `if‑else` 结构。
- 先检查异常条件并 `return` / `throw` / `continue`，主逻辑保持在最外层。
- 正确：
```cpp
// 正确：卫语句
if (nullptr == pObject) return;
if (!pObject->isValid()) return;
pObject->doWork();
```
- 错误：用 `if‑else` 包裹主逻辑，导致主逻辑缩进层级过深。
```cpp
// 错误：深层嵌套
if (nullptr != pObject) {
    if (pObject->isValid()) {
        pObject->doWork();
    }
}
```
- 目的：降低代码嵌套深度，提高可读性，使主逻辑路径更清晰。
- 新增或修改函数时应优先使用卫语句；存量代码如未涉及则不主动批量修改。

### 3.7 禁用制表符（Tab）
- 所有源码文件中**禁止**使用制表符（`\t`），缩进一律使用空格。
- 缩进宽度按现有代码风格（通常 4 空格）。
- 目的：避免不同编辑器/终端对 Tab 显示宽度不一致导致代码错位，保证所有环境下对齐统一。
- 新增或修改文件时必须遵守；存量代码如未涉及则不主动批量转换。

### 3.8 布尔 / 指针条件表达式显式比较（禁止 `!` 取反）
在 `if` / `while` / `else if` 等条件表达式中，**禁止**使用 `!` 取反运算符对布尔值或指针进行判断，必须使用 `true` / `false` / `nullptr` 与 `==` 显式比较，并遵循 §3.5 Yoda Condition（常量左置）。

#### 3.8.1 布尔变量
- 正确：`if (false == bFlag)`、`if (true == bFlag)`
- 错误：`if (!bFlag)`、`if (bFlag)`

#### 3.8.2 函数返回值（返回 `bool`）
包括普通函数、成员函数、链式调用中最终返回 `bool` 的表达式。
- 正确：`if (false == func())`、`if (true == pObj->IsRunning())`、`if (false == vct.empty())`
- 错误：`if (!func())`、`if (!pObj->IsRunning())`、`if (!vct.empty())`

#### 3.8.3 指针空检查
- 正确：`if (nullptr == pCamera)`、`if (nullptr == m_pCtlSel)`
- 错误：`if (!pCamera)`、`if (!m_pCtlSel)`

#### 3.8.4 复合条件中的取反
复合条件（`&&` / `||`）中的子表达式同样适用本规则，每个子项独立遵守。
- 正确：`if (false == bProcess && pSoEvent->getTypeId().isDerivedFrom(...))`
- 错误：`if (!bProcess && pSoEvent->getTypeId().isDerivedFrom(...))`

#### 3.8.5 目的
与 Yoda Condition 风格统一，避免 `!` 在复杂表达式中遗漏或误读，使判断意图一目了然。`!` 容易与 `!=` 混淆，且在长条件链中可读性差。

#### 3.8.6 适用范围
- 新增或修改条件表达式时必须遵循此规则；存量代码如未涉及则不主动批量修改。

### 3.9 禁用 IndpVector 容器
- 新增或修改代码时，**禁止**使用 `IndpVector` 作为容器类型，应使用 `std::vector` 或其他标准库容器替代。
- 目的：`IndpVector` 是历史遗留容器，用户不希望在新代码或改动中继续使用。
- 新增或修改文件时必须遵循此规则；存量代码如未涉及则不主动批量替换。

### 3.10 禁用 `using namespace Uvee;`
- 所有 `.h` 和 `.cpp` 文件中**禁止**单独使用 `using namespace Uvee;`。
- 使用时必须以显式命名空间前缀引用 Uvee 类型（如 `Uvee::SceneNode`、`Uvee::SceneManager`），或仅在 `.cpp` 内使用 `using Uvee::某具体类;` 的局部声明。
- 目的：`Uvee` 是渲染引擎核心命名空间，符号体量巨大；`using namespace Uvee;` 会将大量符号泄漏到当前作用域，增加命名冲突和歧义风险，降低跨仓库/跨引擎的可读性。头文件中尤其危险，会污染所有包含者。
- 新增或修改文件时必须遵循此规则；存量代码如未涉及则不主动批量修改。

### 3.11 `if` / `else` 必须使用大括号
- 所有 `if`、`else if`、`else` 分支**必须**使用 `{}` 包裹，即使只有一条语句也不允许省略。
- 正确：
```cpp
if (nullptr == pEnt)
{
    return false;
}
```
- 错误：`if (x) return;`、`if (x) doSomething();`（无大括号）
- 目的：防止后续维护时因插入新语句导致逻辑错误（`goto fail` 类 bug），同时与仓库整体风格保持一致。
- 新增或修改条件分支时必须遵循此规则；存量代码如未涉及则不主动批量修改。

### 3.12 STL 头文件集中管理
- 各模块的 `PreCompiled.h`（预编译头）中**必须**集中管理该模块用到的所有 STL 头文件（如 `<algorithm>`、`<map>`、`<regex>`、`<iostream>`、`<fstream>` 等）。
- 模块内的 `.cpp` 文件**禁止**单独 `#include` 已在 `PreCompiled.h` 中声明的 STL 头文件。
- 目的：减少重复编译开销，统一模块 STL 依赖管理，便于维护和依赖追踪。
- 新增 `.cpp` 文件时，若需使用 STL 头文件，先检查 `PreCompiled.h` 是否已包含；若未包含，将其添加到 `PreCompiled.h` 的 STL 区域。
- 存量 `.cpp` 文件中若存在已在 `PreCompiled.h` 中的 STL include，可在修改该文件时顺手清理。

### 3.12 头文件函数声明与注释之间必须空行
- 在头文件中，每个函数声明的注释块（`///` 或 `/** */`）与**上一个函数声明**之间**必须**有一个空行。
- 正确：
```cpp
double GetGroundDiv(const MechBase *) const;

/// 1. 获取构件所属楼层或楼层分区标识。
/// 2. 用于按楼层统计、分段取值及规则匹配。
IndpString GetFloorDiv(const MechBase *) const;
```
- 错误：
```cpp
double GetGroundDiv(const MechBase *) const;
/// 1. 获取构件所属楼层或楼层分区标识。
/// 2. 用于按楼层统计、分段取值及规则匹配。
IndpString GetFloorDiv(const MechBase *) const;
```
- 目的：提高头文件的可读性，使函数声明之间的边界更加清晰，便于快速浏览接口定义。
- 新增或修改头文件中的函数声明时必须遵循此规则；存量代码如未涉及则不主动批量修改。

### 3.13 成员变量命名
- 类的成员变量**必须**使用 `m_` 前缀 + 小驼峰命名（lowerCamelCase），即 `m_` 后首个字母小写，后续单词首字母大写。
- 正确：`m_classId`、`m_systemTypeName`、`m_pFittingStart`、`m_vecSupports`
- 错误：`classId`（缺前缀）、`m_ClassId`（大驼峰）、`m_class_id`（snake_case）、`m_classid`（全小写）
- 目的：`m_` 前缀让成员变量在函数体内一眼可辨，避免与局部变量、参数同名混淆；统一的小驼峰风格保持命名一致性。
- 新增或修改成员变量时必须遵循此规则；存量代码如未涉及则不主动批量重命名。

### 3.14 `unordered_map` 自定义键优先特化 `std::hash`
- 当类 / 结构体要作为 `std::unordered_map` / `std::unordered_set` 的键时，**优先**通过 `std::hash<T>` 全特化 + 在类型内定义 `operator==` 的方式支持，而**不是**在类型内嵌套 `Hash` / `Equal` 仿函数再显式传给容器模板。
- 正确：
```cpp
struct MyKey
{
    int m_id;
    bool operator==(const MyKey &o) const { return m_id == o.m_id; }
};

namespace std
{
template <>
struct hash<MyKey>
{
    size_t operator()(const MyKey &k) const noexcept { return std::hash<int>{}(k.m_id); }
};
}

// 调用方：
std::unordered_map<MyKey, Value> data;   // 简洁
```
- 错误：
```cpp
struct MyKey
{
    int m_id;
    struct Hash { ... };    // 嵌套仿函数
    struct Equal { ... };
};
// 调用方：
std::unordered_map<MyKey, Value, MyKey::Hash, MyKey::Equal> data;   // 冗长
```
- 目的：避免容器声明被冗长的模板参数淹没；与 `int` / `std::string` 等标准键的写法保持一致；`std::hash` 是标准库契约，标准容器默认就用它。
- 新增或修改要作 `unordered_map` 键的类型时必须遵循此规则；存量代码如未涉及则不主动批量改造。

- 修改前应先理解相关模块入口、依赖关系和构建方式。

## 4. 搜索与阅读建议
- 优先使用 `rg`、`find`、`sed` 等轻量命令定位代码。
- 检查 `src/` 下已纳管改动时，可优先使用：`svn status src | rg '^[MADRC]'`，用于过滤掉未纳管的 `?` 项。
- 优先查找以下入口文件：
  - `CMakeLists.txt`
  - `*.slnf`
  - `*.vcxproj`
  - 模块级 `CMakeLists.txt`
- 若任务与机电启动、工作台、命令注册相关，优先查看：
  - `src/Mech/MechStart`
  - `src/IndpMajorCtl`
  - `src/QuickModel`
- 涉及 `Mech` 目录的分析、修改或重构时，优先阅读以下文档：
  - `src/Mech/Doc/README.md`
  - `src/Mech/Doc/01‑overview.md`
  - `src/Mech/Doc/02‑module‑dependencies.md`
  - `src/Mech/Doc/03‑architecture‑current.md`
- 若任务涉及 `MechStart` 重构、启动链或命令注册，继续阅读：
  - `src/Mech/Doc/05‑mechstart‑refactor.md`
  - `src/Mech/Doc/06‑mechstart‑implementation‑notes.md`
- 若任务涉及 TD 缺陷处理（`/td` 或 `/td‑commit`），参考 `build_tools/td/README.md`。

### 4.1 禁用 CodeGraph 工具
- **禁止**使用 codegraph 相关工具（MCP），所有代码搜索和阅读一律使用 `rg`、`find`、`Read` 完成。
- 原因：codegraph MCP 调用响应缓慢，影响工作效率。

## 5. 关键模块职责
以下职责总结来自 `src/Mech/Doc` 主文档，分析或改动相关模块时应优先遵守这些边界认知。

### 5.1 `MechStart`
- 定位为入口协调层，不应继续膨胀为大一统业务容器。
- 当前主要职责：
  - 初始化入口
  - GUI 接入点
  - 命令注册协调层
  - Python 模块装载入口
- 修改时优先保持其“瘦入口”方向，避免把业务实现再次堆回入口层。

### 5.2 `MechDesign`
- 负责二次编辑相关业务能力。
- 是体量较大的业务模块。
- 与 `MechIdent` 的边界历史上不够清晰，改动时应避免继续扩大相互耦合。

### 5.3 `MechIdent`
- 负责 CAD 识别相关能力。
- 更适合作为中层能力模块稳定输出。
- 改动时应优先保持识别职责内聚，避免把设计逻辑反向吸入本模块。

### 5.4 `MechCal`
- 负责计算与报表能力。
- 输出形式包含 `.pyd`，其初始化和装载边界要谨慎处理。
- 若改动构建或初始化逻辑，应先确认工程完整性与配置一致性。

### 5.5 `MechData`
- 负责数据相关支撑能力。
- 更适合作为稳定支撑层维护。
- 尽量避免把入口聚合或复杂业务编排职责塞入该模块。

### 5.6 `MechAux`
- 负责通用辅助功能与部分 UI 组件支持。
- `Common/` 侧重辅助对象、枚举操作、集合管理、Viewer 封装。
- `UI/` 侧重模型、流程、样式、视图对话框和表格组件。
- 该模块相对稳定，适合小步维护，不宜引入不必要的业务耦合。

## 6. 推荐架构边界
- 当前 `Mech` 体系可按三层理解：
  - 入口层：`MechStart`
  - 业务层：`MechDesign`、`MechIdent`、`MechCal`
  - 支撑层：`MechData`、`MechAux`
- 修改时应尽量保持“上层依赖下层”的方向感。
- 应避免支撑层重新承担入口聚合职责。
- 应避免把新业务逻辑再次集中到 `MechStart`。

## 7. `MechStart` 专项规则
涉及 `src/Mech/MechStart` 时，除通用规则外，还应优先遵守以下规则。

### 7.1 职责目标
- `MechStart` 的目标是入口协调层，而不是业务实现汇总点。
- 任何新改动都应优先判断：是否应该放在下游业务模块，而不是直接写进 `MechStart`。

### 7.2 初始化链规则
- 优先保持现有阶段边界：
  - `Bootstrap()`
  - `ResourceInit()`
  - `InitCommands()`
  - `InitWorkbench()`
  - `AppInit()`
- 不要把资源初始化、Workbench 初始化、命令注册重新混回主入口。
- 若修改初始化链，应优先保证失败路径清晰、行为可回退、日志语义不变。

### 7.3 命令注册规则
- 避免把新命令继续集中追加到单一文件。
- 优先采用按模块、按职责分组的注册方式。
- 若需要拆分命令注册，先从低耦合、易抽离的命令组开始。
- 高耦合区域应谨慎处理，特别是：
  - `Module`
  - `initModule()`
  - `createOtherCommands()`
- 在边界尚未更清楚前，不要为了“拆分”而强拆这些高风险区域。

### 7.4 实施原则
- 先收口，不扩写。
- 先稳住初始化分层，再推进命令注册去中心化。
- 每一步都尽量保持可编译、可回退、可验证状态。

## 8. 修改禁区与谨慎区
- 非必要不要改动自动生成文件、构建产物和第三方目录。
- 优先避免修改以下目录中的内容，除非用户明确要求：
  - `3rdParty/`
  - `build_x64/`
  - `bin/`
  - `lib/`
  - `libraries/`
  - `install/`
  - `.svn/`
- 若必须改动资源、配置或工程文件，应同步检查关联引用是否一致。
- 若改动 `CMakeLists.txt`、`*.vcxproj`、资源文件或命令注册文件，应特别说明影响范围。

## 9. 注释增强专项规则
当任务是“补注释”“增强注释”“整理说明性注释”时，必须遵守以下规则；如果用户要求与本规则冲突，则以用户要求为准。

### 9.1 全局注释规则
- 只能增加注释，不允许修改任何业务逻辑。
- 不允许修改变量名、函数名、类名、宏名、`#include`。
- 不允许改变代码顺序。
- 不允许删除原有注释。
- 保持原有缩进、空行和代码风格。
- 任何新增说明都必须写成源码注释，不能输出裸文本。
- 对于 C/C++ 文件，新增中文说明必须使用 `///` 或 `/** */`。
- 注释应自然、工程化，避免模板化废话。

### 9.2 头文件注释规则
- 重点说明“接口整体做什么”。
- 优先补充类职责、函数用途、必要时补参数或返回值说明。
- 简单接口尽量压缩为 `1.` `2.` 两条。
- 复杂接口再扩展到 `3.` 或 `4.`，不要机械凑条数。
- 使用单层编号风格：`1.` `2.` `3.` `4.`。
- **仅有一行注释时，不加编号**，直接写为普通注释。
- 头文件中的编号注释可以写在类声明或函数声明外部上方。
- 头文件优先描述接口职责，不要过度写实现细节。

### 9.3 源文件注释规则
- 重点说明“函数内部怎么做”。
- 若头文件已经说明接口职责，源文件中不要重复整段接口说明。
- 每个函数内部的直属步骤编号必须独立从 `1.` 开始。
- 不同函数之间编号不能连续累加。
- 每个大括号块都可视作独立编号作用域。
- 在 `if/else/for/while/switch/case` 等块内，也必须从 `1.` 开始。
- 内层块结束后，编号必须恢复到直接父作用域继续编号。
- 禁止出现 `1.1`、`1.2`、`2.1` 这类多层编号。
- 如果函数只有一条新增注释，也应写成函数体内部的 `1.`。

## 10. 构建与验证
- 本仓库体量较大，验证应先做最小化、定点式检查。
- 若仅修改单个模块，优先做该模块级别的构建或静态检查，不要默认全量编译。
- 如果当前回合用户只是要求分析、扫描或补规则文件，不必主动跑全量构建。
- 若要执行耗时编译、测试或可能改写大量产物的命令，先确认该操作确有必要。
- 若任务集中在 `MechStart`，验证时优先围绕启动链、命令注册、Workbench 初始化与 Python 模块入口做定点检查。

## 11. 提交说明格式
- 本仓库默认使用固定提交说明模板。
- 若用户要求提交代码，优先按以下格式组织提交说明，并只填写 `xxx` 的实际内容。
- 模板如下：
  - `TD: 暂无（xxx）`
  - `问题原因：xxx。`
  - `解决方案：xxx。`
  - `影响范围：xxx。`
  - `代码评审人：当前大模型名称（如 qwen3.7‑max）`
  - `业务评审人：当前大模型名称（如 qwen3.7‑max）`
  - `代码提交版本：主干`
- 若无额外说明，不要擅自改动该模板字段名、顺序和默认值。
- 若本次仅提交单文件或文档，也应沿用该模板。
- **代码评审人和业务评审人**：统一填写当前正在使用的大模型名称（如 qwen3.7‑max、claude‑opus‑4‑8 等），不写"暂无"，便于追溯 AI 辅助提交的模型版本。

## 12. 文档与交付
- 完成修改后，说明改了哪些文件、改动目的、影响范围。
- 若发现潜在风险、历史遗留问题或与本次任务无关但值得注意的问题，可以简要提示，但不要顺手扩改。
- 引用文件时，尽量给出清晰路径，方便用户定位。
- 涉及 `Mech` 相关任务时，尽量先以 `src/Mech/Doc` 主文档为知识入口，而不是优先翻旧档案。