# Claude Code 新电脑部署运行手册：pulseTrader + XAU v10 + LLM overlay

> **读者：新电脑上的 Claude Code。**
>
> 你的任务不是“解释怎么做”，而是按本文顺序**实际执行命令、检查结果、修复路径问题、启动并验收**。
> 遇到需要用户输入的内容，先列出缺失项让用户填写，不要猜 API key，不要把 `.env` 内容打印出来。
>
> 交易边界：只部署 **Gate 模拟盘 / paper account**。绝不发送真实订单，绝不使用真实账户密钥。
>
> 适用仓库：`pulseTrader`（Qt 执行层）+ `gold-trader-mind`（策略仓库，策略在其 `celue` 子目录）。
> `celue` 不在 pulseTrader 仓库里；只 clone pulseTrader 不足以运行 v10。

---

## 0. 开始前：向用户收集这些参数

不要擅自假设盘符。先确认或让用户补齐：

| 参数 | 含义 | 示例 |
|---|---|---|
| `<QT_ROOT>` | pulseTrader 仓库目录 | `D:\quant\pulseTrader` |
| `<QT_REPO>` | pulseTrader git 地址 | `https://github.com/James-Xue/pulseTrader.git` |
| `<QT_BRANCH>` | 要部署的分支 | 以用户提交的分支为准 |
| `<CE_ROOT>` | ce 策略目录（gold-trader-mind 仓库的 `celue` 子目录） | `D:\quant\gold-trader-mind\celue` |
| `<CE_REPO>` | 策略所在 git 仓库 | `https://github.com/James-Xue/gold-trader-mind.git` |
| `<CE_BRANCH>` | 策略仓库分支 | `main` 或用户提交的分支 |
| `<CE_SOURCE>` | 如果不用 git，ce 的复制来源 | 用户提供的共享目录/U 盘路径 |
| `<DATA_ROOT>` | 策略运行数据目录 | `D:\quant\quantification_platform\xau_data` |
| `<STORE_ROOT>` | 1m K 线数据仓库（可选） | `D:\quant\XAUUSD_Data` |
| `<VCPKG_ROOT>` | vcpkg 安装目录 | `C:\vcpkg` |
| `<QT6_DIR>` | Qt6 CMake 目录 | `C:\Qt\6.5.3\msvc2019_64\lib\cmake\Qt6` |
| `<ACCOUNT_MODE>` | 新账户 / 延续旧账户 | `fresh` 或 `migrate` |

用户需要提供的最少信息：

1. 新机器的安装根目录；
2. `gold-trader-mind` 仓库是否已可 clone，或 `celue` 复制路径；
3. Qt 安装路径、vcpkg 路径（如果已安装）；
4. 是否延续旧模拟账户和历史；
5. Gate **模拟/测试** API key（只让用户自己写入 `.env`，不要在对话中发送）。

**Stop here if** `<CE_ROOT>` 没有来源：Qt 仓库不包含策略运行目录，不能只部署 Qt 就宣布完成。

---

## 1. 部署目标结构

最终目录：

```text
<ROOT>\
  pulseTrader\                 # Qt 源码 / git 仓库
    build\apps\pulsetrader_ui\Debug\pulsetrader_ui.exe
    .claude\skills\gate-sim\
    .claude\skills\xau-llm-overlay\
  gold-trader-mind\            # 策略 git 仓库
    celue\                     # ce 策略代码、配置、strategy、prompts
  quantification_platform\
    xau_data\                  # 数据，不提交 git
      sim\                     # Qt 模拟账户、订单历史、外部信号、queue、overlay
      latest_snapshot.json
      live_snapshot.json
      model_fast_log.jsonl
      exec_signals\
      history\
      research\
  XAUUSD_Data\                 # 可选：1m K 线仓库
```

`<CE_ROOT>` 就是：

```text
<ROOT>\gold-trader-mind\celue
```

`gold-trader-mind` 是策略仓库；`celue` 是其中的策略运行目录，不是嵌套 git 仓库。

关键路径关系：

```text
Qt sim_data_dir  = <DATA_ROOT>\sim
Python data_dir  = <DATA_ROOT>
Python work_dir  = <CE_ROOT>
Qt exe           = <QT_ROOT>\build\apps\pulsetrader_ui\Debug\pulsetrader_ui.exe
```

---

## 2. 安装基础环境

### 2.1 必需组件

| 组件 | 要求 |
|---|---|
| Windows | Windows 10/11 64-bit |
| Visual Studio | VS 2022，安装“使用 C++ 的桌面开发” |
| CMake | 3.25+ |
| Qt | 6.5.3 MSVC2019 64-bit，包含 Widgets、Sql、SQLite 驱动 |
| vcpkg | 设置 `VCPKG_ROOT` |
| Python | 3.8+，建议 3.10/3.11；v10 链路只用标准库 |
| Git | 最新版 |

如果用户机器没有这些组件：

- 可以用 `winget` 安装 Git、Python、CMake，但 **Qt 和 Visual Studio Build Tools 通常需要用户确认 GUI/管理员权限**；
- 不要静默安装大型组件；
- 安装完成后重新打开终端，使 PATH 生效。

检查命令：

```powershell
git --version
python --version
cmake --version
$env:VCPKG_ROOT
```

### 2.2 安装 vcpkg（如果缺失）

```powershell
git clone https://github.com/microsoft/vcpkg.git C:\vcpkg
C:\vcpkg\bootstrap-vcpkg.bat
[Environment]::SetEnvironmentVariable("VCPKG_ROOT", "C:\vcpkg", "User")
```

重新打开 PowerShell，确认 `$env:VCPKG_ROOT` 非空。

### 2.3 安装 Qt

使用 Qt Online Installer 安装：

- Qt 6.5.3
- MSVC 2019 64-bit
- Qt Widgets
- Qt Sql
- Qt SQLite driver（`qsqlite.dll`）
- 可选：Qt Debug Information Files

如果 Qt 不在默认目录，设置：

```powershell
$env:QT6_DIR = "C:\Qt\6.5.3\msvc2019_64\lib\cmake\Qt6"
```

**验收：**

```powershell
Test-Path "$env:QT6_DIR\Qt6Config.cmake"
```

---

## 3. 获取代码

### 3.1 Qt 仓库

```powershell
cd <ROOT>
git clone <QT_REPO> pulseTrader
cd pulseTrader
git checkout <QT_BRANCH>
```

如果用户已经在旧机器提交并推送，必须先让用户完成 commit/push；不要部署未提交的本地修改。

### 3.2 ce 策略目录

策略已经放在 `gold-trader-mind` 仓库的 `celue` 子目录。克隆：

```powershell
cd <ROOT>
git clone https://github.com/James-Xue/gold-trader-mind.git gold-trader-mind
cd gold-trader-mind
git checkout <CE_BRANCH>
```

最终目录：

```text
<ROOT>\gold-trader-mind\celue
```

如果用户通过共享盘/U 盘复制，则复制到同一个位置：

```powershell
robocopy "<CE_SOURCE>" "<CE_ROOT>" /E ^
  /XD __pycache__ exec_signals auto_manage reviews reports ^
  /XF *.lock *.pyc *.log *.jsonl *.db *.db-wal *.db-shm ^
      .llm_overlay_tmp_*.json latest_snapshot.json live_snapshot.json
```

不要在没有 `gold-trader-mind` 仓库时假设 `ce` 存在。`celue` 不是独立的嵌套 git 仓库，直接由外层 `gold-trader-mind` 跟踪。

**最小文件必须存在：**

```text
xau_model_fast.py
xau_model.py
xau_levels.py
gold_snapshot.py
xau_manage.py
xau_signal.py
xau_signal_queue.py
xau_llm_overlay.py
xau_paths.py
xau_store_data.py
check_env.py
xau_model_config_selected.json
strategy/xau_5m_strategy.md
data_calendar.json
```

如果缺少任何一项：**停止并报告缺什么**，不要用空文件占位。

---

## 4. 配置路径

### 4.1 创建 `ce/xau_config.json`

从 Qt 仓库的示例复制：

```powershell
Copy-Item "<QT_ROOT>\docs\xau_config.example.json" "<CE_ROOT>\xau_config.json"
```

编辑为实际路径：

```json
{
  "_note": "Machine-specific paths. Environment variables take precedence.",
  "work_dir": "D:/quant/gold-trader-mind/celue",
  "data_dir": "D:/quant/quantification_platform/xau_data",
  "sim_data_dir": "D:/quant/quantification_platform/xau_data/sim",
  "sim_app_dir": "D:/quant/pulseTrader/build/apps/pulsetrader_ui",
  "sim_exe": "D:/quant/pulseTrader/build/apps/pulsetrader_ui/Debug/pulsetrader_ui.exe",
  "analysis_cadence": "1m_all"
}
```

规则：

- 路径使用正斜杠 `/`；
- `work_dir` 指向 ce；
- `data_dir` 指向 xau_data；
- `sim_data_dir` 指向 `xau_data/sim`；
- `sim_app_dir` 是 `.env` 所在目录；
- `sim_exe` 是 Debug 版 Qt 可执行文件。

**不要把中文写进 `_note`**：Windows PowerShell 5.1 可能按旧代码页读取 JSON，导致 `gate-sim` 解析失败并回落到 `Debug\data`。使用 ASCII 说明最稳。

### 4.2 环境变量（可选但推荐）

```powershell
$env:XAU_WORK_DIR = "<CE_ROOT>"
$env:XAU_DATA_DIR = "<DATA_ROOT>"
$env:PULSETRADER_SIM_DATA_DIR = "<DATA_ROOT>\sim"
$env:PULSETRADER_APP_DIR = "<QT_ROOT>\build\apps\pulsetrader_ui"
$env:PULSETRADER_EXE = "<QT_ROOT>\build\apps\pulsetrader_ui\Debug\pulsetrader_ui.exe"
```

环境变量优先于 `xau_config.json`。

### 4.3 创建 `.env`

把模板复制到 Qt 应用工作目录：

```powershell
Copy-Item "<QT_ROOT>\.env.example" "<QT_ROOT>\build\apps\pulsetrader_ui\.env"
```

让用户自己编辑：

```text
<QT_ROOT>\build\apps\pulsetrader_ui\.env
```

填入 Gate **模拟/测试** key：

```dotenv
GATE_MAINNET_API_KEY=...
GATE_MAINNET_API_SECRET=...
```

硬规则：

- 不要读取后打印 `.env`；
- 不要把 `.env` 加到 git；
- 不要用真实账户 key；
- 不要把 key 写进本文档或聊天。

---

## 5. 构建 Qt（Debug only）

先确认没有运行中的旧 exe：

```powershell
Get-Process pulsetrader_ui -ErrorAction SilentlyContinue
```

如果 exe 被占用，关闭 Qt；如仍被锁，改名旧 exe 后重建。

```powershell
cd <QT_ROOT>
$env:VCPKG_ROOT = "<VCPKG_ROOT>"

cmake -B build -S . `
  -DCMAKE_BUILD_TYPE=Debug `
  -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT\scripts\buildsystems\vcpkg.cmake" `
  -DQt6_DIR="<QT6_DIR>" `
  -DPULSE_BUILD_TESTS=OFF `
  -DPULSE_BUILD_UI=ON

cmake --build build --config Debug --target pulsetrader_ui -j 8
```

不要编译 Release，不要构建/运行单元测试（用户约定 Debug 主程序即可）。

**验收：**

```powershell
Test-Path "<QT_ROOT>\build\apps\pulsetrader_ui\Debug\pulsetrader_ui.exe"
```

如果启动时缺 Qt DLL：

```powershell
& "C:\Qt\6.5.3\msvc2019_64\bin\windeployqt.exe" `
  --debug "<QT_ROOT>\build\apps\pulsetrader_ui\Debug\pulsetrader_ui.exe"
```

确认 `sqldrivers\qsqlite.dll` 存在；否则历史交易窗口无法打开。

---

## 6. 安装黄金相关 Claude Code skills

当前只启用并部署这两个：

```text
gate-sim
xau-llm-overlay
```

### 6.1 从 Qt 仓库复制到用户级 skills

项目仓库内路径：

```text
<QT_ROOT>\.claude\skills\gate-sim\
<QT_ROOT>\.claude\skills\xau-llm-overlay\
```

复制到：

```text
%USERPROFILE%\.claude\skills\
```

命令：

```powershell
$dst = Join-Path $env:USERPROFILE ".claude\skills"
New-Item -ItemType Directory -Force $dst | Out-Null
Copy-Item -Recurse -Force "<QT_ROOT>\.claude\skills\gate-sim" (Join-Path $dst "gate-sim")
Copy-Item -Recurse -Force "<QT_ROOT>\.claude\skills\xau-llm-overlay" (Join-Path $dst "xau-llm-overlay")
```

复制前如果目标已存在，先备份：

```powershell
$stamp = Get-Date -Format "yyyyMMdd_HHmmss"
# 例如先 Move-Item 到 gate-sim.backup_<stamp>
```

### 6.2 不要安装/启动的旧 skill

```text
xau-5m
xau-1m
xau-review
```

原因：

- `xau-5m`、`xau-1m` 会写 `gold.exec/1` 订单信号，和 v10 runner 冲突；
- `xau-review` 是可选只读复盘，不属于当前运行链路。

如果旧机器复制了整个 skills 目录，删掉/不要启用这三个，不要把 `xau-5m`、`xau-1m` 加回。

### 6.3 验证 skill

重新打开或重启 Claude Code，让新 skill 被发现。检查：

```powershell
Get-ChildItem (Join-Path $env:USERPROFILE ".claude\skills\gate-sim")
Get-ChildItem (Join-Path $env:USERPROFILE ".claude\skills\xau-llm-overlay")
```

`gate-sim` 启动脚本会读取 `xau_config.json` / 环境变量，并把 `sim_data_dir` 注入 Qt 子进程；`xau-llm-overlay` 使用 `python xau_paths.py` 解析路径。

---

## 7. 数据与账户

### 7.1 新账户（推荐第一次部署）

不复制任何数据，启动 Qt 后通过界面“模拟账户”设置初始余额 100 USDT。

代码会创建：

```text
<DATA_ROOT>\sim\
<DATA_ROOT>\latest_snapshot.json
```

### 7.2 延续旧模拟账户/历史

必须**先停止旧机和新机的 Qt、v10、队列、manager、采集器**，再复制：

```powershell
robocopy "<OLD_DATA_ROOT>\sim" "<NEW_DATA_ROOT>\sim" /E
```

包含：

```text
sim_account.json
sim_orders.db
external_state.json
external_ack.json
external_signals.jsonl
signal_queue.db
llm_overlay.json
llm_overlay_audit.jsonl
xau_5m_heartbeat.json
```

不要复制：

```text
*.lock
*.old_*
build\
.env
```

数据不走 git；`.env` 也不走 git。

---

## 8. 启动顺序

### 8.1 启动 Qt

使用已安装的 skill：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File `
  "$env:USERPROFILE\.claude\skills\gate-sim\scripts\start_gate_sim.ps1"
```

或手工等价启动（必须设置 `PULSETRADER_SIM_DATA_DIR`）：

```powershell
$app = "<QT_ROOT>\build\apps\pulsetrader_ui"
$env:PULSETRADER_SIM_DATA_DIR = "<DATA_ROOT>\sim"
$env:PULSETRADER_APP_DIR = $app
Start-Process `
  -FilePath "$app\Debug\pulsetrader_ui.exe" `
  -ArgumentList @("--auto-login", "--auto-start-engine") `
  -WorkingDirectory $app
```

验收：

```powershell
$state = Get-Content "<DATA_ROOT>\sim\external_state.json" -Raw | ConvertFrom-Json
$state.engine_running
$state.positions.Count
```

`engine_running` 必须为 `True`。如果不是，先修登录/`.env`/网络，不要继续。

### 8.2 启动队列消费者

```powershell
cd <CE_ROOT>
$env:PYTHONDONTWRITEBYTECODE = "1"
Start-Process python -ArgumentList @(
  "xau_signal_queue.py", "--loop", "--ack-timeout", "15"
) -WorkingDirectory "<CE_ROOT>" -WindowStyle Hidden
```

只允许一个消费者。重复启动会被锁拒绝。

### 8.3 启动 v10

```powershell
Start-Process python -ArgumentList @(
  "xau_model_fast.py",
  "--loop",
  "--entry-interval", "60",
  "--level-dist", "0.5",
  "--strategy-id", "v10",
  "--signal-queue"
) -WorkingDirectory "<CE_ROOT>" -WindowStyle Hidden
```

验收日志：

```powershell
Get-Content "<DATA_ROOT>\model_fast_log.jsonl" -Tail 10
```

应看到：

```json
{"event":"fast_start","strategy_id":"v10","signal_queue":true,...}
```

以及后续：

```text
[queue] enqueued ... mode=paper
[ack] status=accepted
[queue] queue_status=accepted
```

如果长期 `mode=shadow`，说明 Qt 引擎状态不新鲜或 `sim_data_dir` 不一致。

### 8.4 可选：启动 K 线采集器

```powershell
Start-Process python -ArgumentList @(
  "xau_store_data.py",
  "--store-dir", "<STORE_ROOT>",
  "--source-dir", "<DATA_ROOT>",
  "--sim-data-dir", "<DATA_ROOT>\sim",
  "--loop", "--interval", "300"
) -WorkingDirectory "<CE_ROOT>" -WindowStyle Hidden
```

一个数据仓库只运行一个 collector。

### 8.5 可选：LLM overlay

在 Claude Code 中，独立会话运行：

```text
/loop 5m /xau-llm-overlay
```

规则：

- 只写顾问 JSON，不下单；
- 不要同时运行 `/xau-5m` 或 `/xau-1m`；
- overlay 缺失/过期时 v10 按原逻辑运行；
- v10 默认 `--llm-mode veto`，LLM 只能否决，不能替代策略。

---

## 9. 验收清单

### 9.1 路径与环境

```powershell
cd <CE_ROOT>
python -B check_env.py
```

必须看到：

```text
PASS — 0 blocking issue(s)
```

并确认：

```text
data_dir = <DATA_ROOT>
sim_data_dir = <DATA_ROOT>\sim
```

`WARN notes` 不是阻塞项；如果启用每日复盘再补 `market_notes.md`。

### 9.2 队列

```powershell
python -B xau_signal_queue.py --status
```

应出现 `accepted`。如果长期 `processing`：

1. 确认 Qt 在运行；
2. 确认只有一个 queue consumer；
3. 重启 consumer（启动时会恢复 processing）。

### 9.3 Qt 数据

```powershell
Get-ChildItem "<DATA_ROOT>\sim" | Select-Object Name
```

应存在：

```text
sim_account.json
sim_orders.db
external_state.json
signal_queue.db
llm_overlay.json（如启用）
```

旧的回落后备目录：

```text
<QT_ROOT>\build\apps\pulsetrader_ui\Debug\data
```

应为空。如果 Qt 写到这里，说明 `PULSETRADER_SIM_DATA_DIR` 没有注入，必须修复后再继续。

### 9.4 进程

正常应只有一个：

```text
pulsetrader_ui.exe
xau_signal_queue.py --loop
xau_model_fast.py --loop ... --strategy-id v10 --signal-queue
```

可选一个：

```text
xau_store_data.py --loop --interval 300
```

### 9.5 历史交易

打开 Qt 的“历史交易”窗口，确认能读取 `<DATA_ROOT>\sim\sim_orders.db`；如果是新账户则历史为空，这是正常。

---

## 10. 常见故障

| 现象 | 原因 | 处理 |
|---|---|---|
| Qt 登录成功但引擎没启动 | `.env` 不在 `sim_app_dir`，key 无 TradFi 权限，网络/代理 | 检查 `.env` 位置和网络；不要打印 key |
| Qt 写到 `Debug\data` | `PULSETRADER_SIM_DATA_DIR` 没注入 | 用 `gate-sim` 脚本，或手工设置 env 后重启 Qt |
| `gate-sim` 配置解析失败 | `xau_config.json` 编码/中文说明导致 PowerShell 5.1 解析失败 | 使用 ASCII `_note`；脚本用 UTF-8 读取 |
| v10 长期 shadow | Qt 没运行/状态过期/sim_data_dir 不一致 | 先启动 Qt，检查 `engine_running` |
| 队列 ack timeout | Qt 未处理信号、信号过期、consumer 多开 | 确认 Qt 和单 consumer；重启 consumer |
| 历史交易窗口打不开 | Qt 缺 Sql/`qsqlite.dll` | 安装 Qt Sql，运行 `windeployqt` |
| 构建 exe 被占用 | Qt 还在运行 | 关闭 Qt；必要时重命名旧 exe |
| `__pycache__` 反复出现 | Python 运行产生 | 用 `PYTHONDONTWRITEBYTECODE=1`；`.gitignore` 已忽略 |
| `.llm_overlay_tmp` 出现在 ce | LLM loop 临时文件 | 让临时文件写到 `%TEMP%`；`.gitignore` 已忽略 |

---

## 11. 禁止事项

- 不要发送真实 Gate 订单；只部署 paper account。
- 不要提交 `.env`、API key、`xau_data`、`sim_orders.db`、日志、构建产物。
- 不要直接编辑 `sim_account.json` 或 `sim_orders.db`。
- 不要同时启动 `xau-5m` / `xau-1m` 和 v10。
- 不要在没有验证 `engine_running=True` 的情况下启动 v10。
- 不要把 `.env` 内容打印到终端或写入日志。
- 不要用 `git add -A` 把运行时数据带进仓库。

---

## 12. 最终向用户报告

部署完成后，报告以下信息，不要报告 API key：

```text
Qt exe: <QT_ROOT>\build\apps\pulsetrader_ui\Debug\pulsetrader_ui.exe
CE: <CE_ROOT>
DATA: <DATA_ROOT>
SIM: <DATA_ROOT>\sim

Qt build: OK/FAIL
Qt engine: running/not running
positions: N
queue: accepted
v10: running, signal_queue=true
skills: gate-sim + xau-llm-overlay installed
LLM overlay: enabled/disabled
check_env: PASS/FAIL
```

如果任何一步失败，停在失败点，报告命令、退出码和关键错误；不要继续启动下游进程。
