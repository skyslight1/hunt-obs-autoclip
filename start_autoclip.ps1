# 一键安装+启动 hunt-obs-autoclip（首次运行自动建 venv 装依赖）
# 用法：
#   .\start_autoclip.ps1                          # 正式运行（读 OBS_WS_PASSWORD 环境变量）
#   .\start_autoclip.ps1 -ObsPassword "xxxx"      # 指定 obs-websocket 密码
#   .\start_autoclip.ps1 -DryRun                  # 空跑（不碰 OBS，只看事件）
#   .\start_autoclip.ps1 -ObsPort 4455 -CoalesceGap 15
param(
    [string]$ObsHost = "127.0.0.1",
    [int]$ObsPort = 4455,
    [string]$ObsPassword = $env:OBS_WS_PASSWORD,
    [double]$CoalesceGap = 15,
    [double]$MinInterval = 3,
    [string]$Events = "HUNTER_KILLED,PLAYER_DOWNED",
    [switch]$DryRun,
    [switch]$VerboseLog
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$venvPython = Join-Path $root ".venv\Scripts\python.exe"

# 1. venv + 依赖（幂等：已装则跳过）
if (-not (Test-Path $venvPython)) {
    Write-Host "[1/2] 创建虚拟环境并安装依赖（protobuf / pywin32 / obsws-python）..." -ForegroundColor Cyan
    python -m venv (Join-Path $root ".venv")
    & $venvPython -m pip install --quiet "protobuf==7.35.1" pywin32 obsws-python
    if ($LASTEXITCODE -ne 0) { throw "依赖安装失败，检查网络或 Python" }
} else {
    Write-Host "[1/2] 虚拟环境已就绪，跳过安装" -ForegroundColor DarkGray
}

# 2. 组装参数并前台运行（Ctrl+C 退出）
Write-Host "[2/2] 启动 hunt-obs-autoclip（先启动本工具，再启动游戏）" -ForegroundColor Cyan
$args = @(
    (Join-Path $root "hunt_obs_autoclip.py"),
    "--obs-host", $ObsHost, "--obs-port", $ObsPort,
    "--coalesce-gap", $CoalesceGap, "--min-interval", $MinInterval,
    "--events", $Events
)
if ($ObsPassword) { $args += @("--obs-password", $ObsPassword) }
if ($DryRun)      { $args += "--dry-run" }
if ($VerboseLog)  { $args += "--verbose" }

& $venvPython @args
exit $LASTEXITCODE
