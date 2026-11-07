# run.ps1 — pulseTrader 启动脚本
# 用法: .\run.ps1 trade

$ErrorActionPreference = "Stop"
Set-Location (Split-Path -Parent $MyInvocation.MyCommand.Path)

# Gate.io 测试网凭证:从环境读取(勿硬编码)
# ⚠️ 2026-08-23 隐私扫描:此文件历史版本硬编码过测试网 key/secret 并已推送
# 公开仓库(历史仍可见)—— 请在 Gate 后台轮换测试网密钥;主网密钥从未入库
$env:PULSE_WEBUI_TOKEN       = 'demo'
$env:HTTPS_PROXY             = 'http://127.0.0.1:7897'
$env:HTTP_PROXY              = 'http://127.0.0.1:7897'

$exe  = "build\apps\pulsetrader\Release\pulsetrader.exe"
$tool = "build\tools\Release"

$cmd = if ($args.Count -gt 0) { $args[0] } else { "" }

switch ($cmd) {
    "trade" {
        Write-Host "=== pulseTrader Trading Engine ===" -ForegroundColor Cyan
        # Auto-open WebUI browser after a short delay (server starts on port 8080)
        Start-Job -ScriptBlock {
            Start-Sleep -Seconds 5
            Start-Process "http://127.0.0.1:8080?token=demo"
        } | Out-Null
        if (Test-Path trading.toml) {
            Write-Host "[OK] Auto-loading trading.toml" -ForegroundColor Green
            Write-Host "[OK] WebUI will open at http://127.0.0.1:8080?token=demo" -ForegroundColor Green
            Write-Host ""
            & $exe --config trading.toml
        } else {
            & $exe
        }
    }
    "rest"     { & "$tool\test_gate_rest.exe" }
    "ws"       { & "$tool\test_gate_ws.exe" }
    "market"   { & "$tool\test_market_feed.exe" }
    "strategy" { & "$tool\test_strategy.exe" }
    "ai"       { & "$tool\test_ai_pipeline.exe" --mock }
    "webui"    {
        Write-Host "=== pulseTrader WebUI Server ===" -ForegroundColor Cyan
        $proc = Start-Process -FilePath "$tool\test_webui_server.exe" -PassThru -NoNewWindow
        Start-Sleep -Seconds 2
        Start-Process "http://127.0.0.1:8080?token=demo"
        Write-Host ""
        Write-Host "Dashboard: http://127.0.0.1:8080?token=demo" -ForegroundColor Green
        Write-Host "Press Ctrl+C to stop the server" -ForegroundColor Yellow
        Write-Host ""
        try { Wait-Process -Id $proc.Id } catch {}
    }
    "test"     { Push-Location build; ctest -C Release --output-on-failure; Pop-Location }
    default    {
        Write-Host "Usage: .\run.ps1 {trade|rest|ws|market|strategy|ai|webui|test}"
    }
}
