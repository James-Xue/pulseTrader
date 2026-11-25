# start_gate_sim.ps1 - launch or focus the local Qt "模拟gate系统" app.
#
# The launcher passes --auto-login (credentials are read from .env) and
# --auto-start-engine, so a fresh launch logs in and starts polling without
# any clicks. If xau-5m is not running yet the app keeps polling prices and
# starts consuming external signals as soon as its heartbeat appears.
#
# The working directory matters: the app reads .env from
# <repo>\build\apps\pulsetrader_ui, while sim_account.json lives in
# <repo>\build\apps\pulsetrader_ui\Debug\data. Launching from the exe's own
# folder (Explorer double-click) would use a different .env search path.
$ErrorActionPreference = 'Stop'

# Portable config: XAU_WORK_DIR/xau_config.json is the preferred source.
# Environment variables always win over the config.
$configCandidates = @()
if ($env:XAU_WORK_DIR) {
    $configCandidates += (Join-Path $env:XAU_WORK_DIR 'xau_config.json')
}
$configCandidates += @(
    (Join-Path $PSScriptRoot 'xau_config.json'),
    (Join-Path $PSScriptRoot '..\..\..\xau_config.json'),
    (Join-Path $PSScriptRoot '..\..\..\..\xau_config.json')
)
$cfg = $null
foreach ($c in $configCandidates) {
    if (Test-Path $c) { try { $cfg = Get-Content -Raw -Encoding UTF8 $c | ConvertFrom-Json; break } catch { } }
}

$exe = if ($env:PULSETRADER_EXE) { $env:PULSETRADER_EXE }
       elseif ($cfg -and $cfg.sim_exe) { $cfg.sim_exe }
       else { '' }
$cwd = if ($env:PULSETRADER_APP_DIR) { $env:PULSETRADER_APP_DIR }
       elseif ($cfg -and $cfg.sim_app_dir) { $cfg.sim_app_dir }
       else { '' }
$dataDir = if ($env:PULSETRADER_SIM_DATA_DIR) { $env:PULSETRADER_SIM_DATA_DIR }
           elseif ($cfg -and $cfg.sim_data_dir) { $cfg.sim_data_dir }
           else { '' }
if (-not $exe -or -not $cwd -or -not $dataDir) {
    Write-Error 'gate-sim: path config missing. Set XAU_WORK_DIR (with xau_config.json) or PULSETRADER_EXE / PULSETRADER_APP_DIR / PULSETRADER_SIM_DATA_DIR.'
    exit 2
}
$heartbeat = Join-Path $dataDir 'xau_5m_heartbeat.json'
$statePath = Join-Path $dataDir 'external_state.json'

function Write-HeartbeatStatus {
    if (-not (Test-Path $heartbeat)) {
        Write-Output 'xau-5m: heartbeat missing - the app polls only until xau-5m starts.'
        return
    }
    try {
        $hb = Get-Content -Raw -Encoding UTF8 $heartbeat | ConvertFrom-Json
        $updated = [datetimeoffset]::Parse([string]$hb.updated_at)
        $age = ([datetimeoffset]::Now - $updated).TotalSeconds
        if ($age -le 720) {
            Write-Output ('xau-5m: heartbeat fresh ({0:N0}s old, mode={1}).' -f $age, $hb.mode)
        } else {
            Write-Output ('xau-5m: heartbeat stale ({0:N0}s old) - the app keeps polling and will pick it up when the loop restarts.' -f $age)
        }
    } catch {
        Write-Output 'xau-5m: heartbeat unreadable - the app polls only until xau-5m starts.'
    }
}

function Get-EngineStatus {
    if (-not (Test-Path $statePath)) {
        return 'engine: not started (no external_state.json yet)'
    }
    try {
        $state = Get-Content -Raw -Encoding UTF8 $statePath | ConvertFrom-Json
        $updated = [datetimeoffset]::Parse([string]$state.updated_at)
        $age = ([datetimeoffset]::Now - $updated).TotalSeconds
        $positions = 0
        if ($null -ne $state.positions) { $positions = @($state.positions).Count }
        if ($state.engine_running -and $age -le 15) {
            return ('engine: running (state {0:N0}s old, positions={1})' -f $age, $positions)
        }
        return ('engine: not confirmed (engine_running={0}, state {1:N0}s old)' -f $state.engine_running, $age)
    } catch {
        return 'engine: state unreadable'
    }
}

function Wait-Engine([int]$seconds) {
    $deadline = [datetime]::Now.AddSeconds($seconds)
    while ([datetime]::Now -lt $deadline) {
        if (Test-Path $statePath) {
            try {
                $state = Get-Content -Raw -Encoding UTF8 $statePath | ConvertFrom-Json
                $updated = [datetimeoffset]::Parse([string]$state.updated_at)
                $age = ([datetimeoffset]::Now - $updated).TotalSeconds
                if ($state.engine_running -and $age -le 15) {
                    return $true
                }
            } catch {
                # State file can be mid-write; retry.
            }
        }
        Start-Sleep -Milliseconds 500
    }
    return $false
}

$running = Get-Process -Name 'pulsetrader_ui' -ErrorAction SilentlyContinue |
    Sort-Object StartTime -Descending | Select-Object -First 1
if ($running) {
    try {
        Add-Type -AssemblyName Microsoft.VisualBasic
        [Microsoft.VisualBasic.Interaction]::AppActivate($running.Id) | Out-Null
    } catch {
        # Activation is best-effort; the process is still reported below.
    }
    Write-Output ('gate-sim: already running PID={0}' -f $running.Id)
    Write-Output (Get-EngineStatus)
    Write-HeartbeatStatus
    Write-Output 'gate-sim: existing instance was focused; it is not restarted.'
    exit 0
}

if (-not (Test-Path $exe)) {
    Write-Error ('gate-sim: executable not found: ' + $exe)
    exit 2
}

# Make the Qt child use the same sim data directory as the Python runners.
# Without this, SimAccountStore falls back to <exe>\data and the account would
# split from the strategy/queue.
$env:PULSETRADER_SIM_DATA_DIR = $dataDir
$env:PULSETRADER_APP_DIR = $cwd

$proc = Start-Process -FilePath $exe `
    -ArgumentList @('--auto-login', '--auto-start-engine') `
    -WorkingDirectory $cwd -PassThru
Write-Output ('gate-sim: started PID={0}' -f $proc.Id)
Write-Output ('gate-sim: cwd=' + $cwd)
Write-Output 'gate-sim: auto-login from .env; auto-start engine requested.'

$engineOk = Wait-Engine 25
$alive = Get-Process -Id $proc.Id -ErrorAction SilentlyContinue
if ($null -eq $alive) {
    Write-Error ('gate-sim: process exited (PID {0}) - check login/network.' -f $proc.Id)
    exit 3
}

if ($engineOk) {
    Write-Output 'gate-sim: login ok, engine running.'
} else {
    Write-Output 'gate-sim: engine not confirmed within 25s - the login window may be waiting (missing/invalid .env credentials or network).'
}
Write-Output (Get-EngineStatus)
Write-HeartbeatStatus
exit 0
