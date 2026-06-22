# VS Code Development Setup Guide

## Required Extensions

Install the following VS Code extensions:

1. **C/C++** (Microsoft) — IntelliSense, debugging, code navigation
2. **CMake Tools** (Microsoft) — CMake build integration
3. **CodeLLDB** (optional) — Alternative debugger to cppdbg

## Quick Start

### 1. Open the project
```bash
code /home/joey/1_Code/09_pulseTrader
```

### 2. Configure CMake
- Press `Ctrl+Shift+P` → type `CMake: Configure`
- Or click the "Configure" button in the status bar
- CMake Tools will auto-detect the compiler and configure the project

### 3. Build the project
- Press `F7` or `Ctrl+Shift+B` to build
- Or click the "Build" button in the status bar
- Build output goes to the `build/` directory

### 4. Debug the program

#### Pre-configured debug targets (select with F5):
- **Debug: REST API Test** — Test Gate.io REST endpoints
- **Debug: WebSocket Test** — Test WebSocket connection
- **Debug: Market Feed** — Test market data pipeline
- **Debug: Strategy Engine** — Test strategy engine
- **Debug: AI Pipeline (Mock)** — Test AI pipeline (mock mode)
- **Debug: WebUI Server** — Start WebUI server
- **Debug: Unit Tests (CTest)** — Run all unit tests

#### Debug steps:
1. Set breakpoints in code (click left of line number)
2. Press `F5` or click the "Run and Debug" panel
3. Select the debug configuration to run
4. Program will pause at breakpoints

### 5. Run tests
- Press `Ctrl+Shift+P` → `Tasks: Run Task` → `Run All Tests`
- Or run in terminal: `./run.sh test`

## Environment Variables

The project requires a `.env` file for API credentials and proxy settings:
```bash
GATE_API_KEY=your_key
GATE_API_SECRET=your_secret
HTTP_PROXY=http://127.0.0.1:7897
HTTPS_PROXY=http://127.0.0.1:7897
```

**Note**: `.env` is not auto-loaded during debugging. For environment variables, add them to the `environment` field in `launch.json`, or manually `source .env` in terminal before launching VS Code.

## IntelliSense Configuration

CMake Tools auto-generates `compile_commands.json`, which IntelliSense uses for:
- Code completion
- Go to definition (F12)
- Find references (Shift+F12)
- Hover documentation

## Keyboard Shortcuts

| Action | Shortcut |
|--------|----------|
| Build | F7 / Ctrl+Shift+B |
| Debug | F5 |
| Set breakpoint | F9 |
| Step over | F10 |
| Step into | F11 |
| Step out | Shift+F11 |
| Continue | F5 |
| Go to definition | F12 |
| Find references | Shift+F12 |
| CMake Configure | Ctrl+Shift+P → "CMake: Configure" |
| Switch build type | Ctrl+Shift+P → "CMake: Select Build Type" |

## Build Types

Switch between Debug/Release:
- `Ctrl+Shift+P` → `CMake: Select Build Type`
- Select `Debug` (debug symbols) or `Release` (optimized)

## Troubleshooting

### IntelliSense not working
1. Ensure CMake Configure has been run
2. Check that `build/compile_commands.json` exists
3. Reload window: `Ctrl+Shift+P` → `Developer: Reload Window`

### Debugger won't start
1. Ensure `gdb` is installed: `sudo apt install gdb`
2. Check build type is Debug (includes `-g` flag)
3. Check debug logs in VS Code Output panel

### Build fails
1. Run `CMake: Clean Reconfigure`
2. Delete `build/` directory and reconfigure
3. Check dependencies: `dpkg -l | grep -E "nlohmann|spdlog|fmt|curl|websocket"`

## Advanced Usage

### Adding new debug configurations
Edit `.vscode/launch.json`, copy an existing configuration and modify the `program` path.

### Custom CMake arguments
Edit the `cmake.configureArgs` array in `.vscode/settings.json`.

### Enable WebUI build
Currently enabled (`-DPULSE_ENABLE_WEBUI=ON`). To disable, modify settings.json.

### View CMake cache
`Ctrl+Shift+P` → `CMake: Edit CMake Cache (UI)`

## Project Structure

```
pulseTrader/
├── src/              # Library source (9 layers)
│   ├── exchange/     # L1: Gate.io REST/WS
│   ├── logging/      # L2: spdlog
│   ├── market/       # L3: Market data
│   ├── ai/           # L4: AI analysis
│   ├── heartbeat/    # L5: Scheduler
│   ├── strategy/     # L6: Strategy engine
│   ├── risk/         # L7: Risk management
│   ├── execution/    # L8: Order execution
│   └── webui/        # L9: WebUI
├── tests/            # Unit tests (357)
├── tools/            # Test/demo tools
├── apps/             # Main application
├── docs/             # Documentation
└── .vscode/          # VS Code configuration
```

## References

- [CMake Tools docs](https://github.com/microsoft/vscode-cmake-tools/blob/main/docs/README.md)
- [C/C++ extension docs](https://code.visualstudio.com/docs/cpp/)
- Project architecture: `docs/architecture.md`
- Implementation roadmap: `docs/implementation-roadmap.md`
