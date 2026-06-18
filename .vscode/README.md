# VS Code 开发环境配置指南

## 必需扩展

安装以下 VS Code 扩展：

1. **C/C++** (Microsoft) — IntelliSense、调试、代码导航
2. **CMake Tools** (Microsoft) — CMake 构建集成
3. **CodeLLDB** (可选) — 替代 cppdbg 的调试器

## 快速开始

### 1. 打开项目
```bash
code /home/joey/1_Code/09_pulseTrader
```

### 2. 配置 CMake
- 按 `Ctrl+Shift+P` → 输入 `CMake: Configure`
- 或点击状态栏的 "Configure" 按钮
- CMake Tools 会自动检测编译器并配置项目

### 3. 构建项目
- 按 `F7` 或 `Ctrl+Shift+B` 构建
- 或点击状态栏的 "Build" 按钮
- 构建输出在 `build/` 目录

### 4. 调试程序

#### 预设调试配置（按 F5 选择）：
- **Debug: REST API Test** — 测试 Gate.io REST 接口
- **Debug: WebSocket Test** — 测试 WebSocket 连接
- **Debug: Market Feed** — 测试行情数据管道
- **Debug: Strategy Engine** — 测试策略引擎
- **Debug: AI Pipeline (Mock)** — 测试 AI 管道（mock 模式）
- **Debug: WebUI Server** — 启动 WebUI 服务器
- **Debug: Unit Tests (CTest)** — 运行所有单元测试

#### 调试步骤：
1. 在代码中设置断点（点击行号左侧）
2. 按 `F5` 或点击 "Run and Debug" 面板
3. 选择要调试的配置
4. 程序会在断点处暂停

### 5. 运行测试
- 按 `Ctrl+Shift+P` → `Tasks: Run Task` → `Run All Tests`
- 或在终端运行：`./run.sh test`

## 环境变量

项目需要 `.env` 文件提供 API 凭据和代理设置：
```bash
GATE_API_KEY=your_key
GATE_API_SECRET=your_secret
HTTP_PROXY=http://127.0.0.1:7897
HTTPS_PROXY=http://127.0.0.1:7897
```

**注意**：调试时 `.env` 不会自动加载。如需环境变量，在 `launch.json` 的 `environment` 字段添加，或使用终端手动 `source .env` 后启动 VS Code。

## IntelliSense 配置

CMake Tools 会自动生成 `compile_commands.json`，IntelliSense 会据此提供：
- 代码补全
- 跳转到定义（F12）
- 查找引用（Shift+F12）
- 悬停文档

## 常用快捷键

| 功能 | 快捷键 |
|------|--------|
| 构建 | F7 / Ctrl+Shift+B |
| 调试 | F5 |
| 设置断点 | F9 |
| 单步执行 | F10 |
| 进入函数 | F11 |
| 跳出函数 | Shift+F11 |
| 继续执行 | F5 |
| 跳转到定义 | F12 |
| 查找引用 | Shift+F12 |
| CMake Configure | Ctrl+Shift+P → "CMake: Configure" |
| 切换构建类型 | Ctrl+Shift+P → "CMake: Select Build Type" |

## 构建类型

切换 Debug/Release：
- `Ctrl+Shift+P` → `CMake: Select Build Type`
- 选择 `Debug`（调试符号）或 `Release`（优化）

## 故障排除

### IntelliSense 不工作
1. 确保已运行 CMake Configure
2. 检查 `build/compile_commands.json` 是否存在
3. 重新加载窗口：`Ctrl+Shift+P` → `Developer: Reload Window`

### 调试器无法启动
1. 确保已安装 `gdb`：`sudo apt install gdb`
2. 检查构建类型为 Debug（包含 `-g` 标志）
3. 查看 VS Code 输出面板的调试日志

### 构建失败
1. 运行 `CMake: Clean Reconfigure`
2. 删除 `build/` 目录后重新配置
3. 检查依赖是否安装：`dpkg -l | grep -E "nlohmann|spdlog|fmt|curl|websocket"`

## 高级用法

### 添加新的调试配置
编辑 `.vscode/launch.json`，复制现有配置并修改 `program` 路径。

### 自定义 CMake 参数
编辑 `.vscode/settings.json` 的 `cmake.configureArgs` 数组。

### 启用 WebUI 构建
当前已启用 (`-DPULSE_ENABLE_WEBUI=ON`)。如需禁用，修改 settings.json。

### 查看 CMake 缓存
`Ctrl+Shift+P` → `CMake: Edit CMake Cache (UI)`

## 项目结构

```
pulseTrader/
├── src/              # 库源码（9 层）
│   ├── exchange/     # L1: Gate.io REST/WS
│   ├── logging/      # L2: spdlog
│   ├── market/       # L3: 行情数据
│   ├── ai/           # L4: AI 分析
│   ├── heartbeat/    # L5: 调度器
│   ├── strategy/     # L6: 策略引擎
│   ├── risk/         # L7: 风控
│   ├── execution/    # L8: 订单执行
│   └── webui/        # L9: WebUI
├── tests/            # 单元测试（357 个）
├── tools/            # 测试/演示工具
├── apps/             # 主程序（待实现）
├── docs/             # 文档
└── .vscode/          # VS Code 配置
```

## 参考

- [CMake Tools 文档](https://github.com/microsoft/vscode-cmake-tools/blob/main/docs/README.md)
- [C/C++ 扩展文档](https://code.visualstudio.com/docs/cpp/)
- 项目架构：`docs/architecture.md`
- 实现路线图：`docs/implementation-roadmap.md`
