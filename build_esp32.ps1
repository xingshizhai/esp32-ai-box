<#
.SYNOPSIS
    加载 ESP-IDF 配置并激活环境（优先 EIM），再编译工程，或仅激活供手动使用 idf.py。

.DESCRIPTION
    配置查找顺序（命中其一即可）：
      1) 环境变量 ESP_IDF_BUILD_CONFIG = 某个 .ps1 的完整路径
      2) 与本脚本同目录的 idf-env.ps1
      3) %USERPROFILE%\.esp-idf-build.ps1
    模板见 idf-env.example.ps1；须设置 $IDF_PATH，建议与 EIM 终端中的 IDF_TOOLS_PATH、IDF_PYTHON_ENV_PATH 一致。

    激活：若 %IDF_TOOLS_PATH%\eim_idf.json（或 C:\Espressif\tools\eim_idf.json）中记录的 path 与 $IDF_PATH
    匹配，则点源其中的 activationScript；否则点源 $IDF_PATH\export.ps1。

.PARAMETER ActivateOnly
    仅加载配置、激活 ESP-IDF 并将当前目录切换到工程目录（$ProjectDir），不执行 idf.py build。
    在交互式会话中请用点源调用（见下方「使用说明」），以便环境保留在当前窗口。

.NOTES
    使用说明（必读）
    --------------
    1) 点源「.」与普通执行「.\」的区别
       - 点源：. .\build_esp32.ps1  — 在当前会话作用域内执行，PATH 等环境变量会保留，适合「激活后继续敲 idf.py」。
       - 普通执行：.\build_esp32.ps1 — 子作用域，脚本结束后对 Path 的修改不会留在你的交互窗口里。

    2) 一条 powershell -Command 里「先激活再 idf.py …」
       必须把激活与后续命令写在同一进程、同一条命令里，且用点源开头，例如：
         powershell -NoProfile -Command ". '…\build_esp32.ps1' -ActivateOnly; idf.py build"
       切勿单独新开进程只跑 idf.py（新进程没有 ESP-IDF 环境）。
       -ActivateOnly 分支结束时使用 return（而非 exit），否则点源脚本里的 exit 会结束整个 powershell 进程，
       分号后面的 idf.py 永远不会执行（表现为无编译、build/log 无新文件）。

    3) 自动化 / OpenCode / CI 的两种推荐写法
       a) 激活后执行自定义命令（如 build、flash）：
          powershell -NoProfile -Command ". 'D:\…\parrot-buddy\build_esp32.ps1' -ActivateOnly; idf.py build"
       b) 仅需要本仓库默认的 idf.py build（无需再接其他命令）：
          powershell -NoProfile -Command ". 'D:\…\parrot-buddy\build_esp32.ps1'"
          即不传 -ActivateOnly，脚本内会执行 idf.py build。

    4) 查看完整帮助
       Get-Help .\build_esp32.ps1 -Full

.EXAMPLE
    PS> cd D:\Work\esp32\projects\parrot-buddy
    PS> .\build_esp32.ps1
    完整编译（idf.py build）。

.EXAMPLE
    PS> . .\build_esp32.ps1 -ActivateOnly
    PS> idf.py menuconfig
    仅激活并进入工程目录；点源后可在同一窗口继续使用 idf.py。

.EXAMPLE
    powershell -NoProfile -Command ". 'D:\Work\esp32\projects\parrot-buddy\build_esp32.ps1' -ActivateOnly; idf.py build"
    同一进程内先激活再编译（适用于 OpenCode、CI）。可将 idf.py build 换成 idf.py flash 等。

.EXAMPLE
    powershell -NoProfile -Command ". 'D:\Work\esp32\projects\parrot-buddy\build_esp32.ps1'"
    一条命令完成激活与本脚本默认的 idf.py build（无需 -ActivateOnly）。

.EXAMPLE
    build_esp32.bat
    build_esp32.bat -ActivateOnly
    CMD 下调用；参数会传给 build_esp32.ps1。
#>

param(
    [switch]$ActivateOnly
)

$ScriptRoot = $PSScriptRoot
if (-not $ScriptRoot) {
    $ScriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
}

$configPath = $null

# If IDF_PATH already set by parent (e.g. build_esp32.bat loaded idf-env.bat), use it
if ($env:IDF_PATH -and (Test-Path -LiteralPath $env:IDF_PATH)) {
    $IDF_PATH = $env:IDF_PATH
    if ($env:IDF_TOOLS_PATH) { $IDF_TOOLS_PATH = $env:IDF_TOOLS_PATH }
    if ($env:IDF_PYTHON_ENV_PATH) { $IDF_PYTHON_ENV_PATH = $env:IDF_PYTHON_ENV_PATH }
    Write-Host "使用环境变量 IDF_PATH: $IDF_PATH" -ForegroundColor Cyan
}
elseif ($env:ESP_IDF_BUILD_CONFIG -and (Test-Path -LiteralPath $env:ESP_IDF_BUILD_CONFIG)) {
    $configPath = $env:ESP_IDF_BUILD_CONFIG
}
elseif (Test-Path -LiteralPath (Join-Path $ScriptRoot "idf-env.ps1")) {
    $configPath = Join-Path $ScriptRoot "idf-env.ps1"
}
elseif (Test-Path -LiteralPath (Join-Path $env:USERPROFILE ".esp-idf-build.ps1")) {
    $configPath = Join-Path $env:USERPROFILE ".esp-idf-build.ps1"
}

if ($configPath) {
    Write-Host "加载配置: $configPath" -ForegroundColor Cyan
    . $configPath
}

if (-not $IDF_PATH) {
    Write-Host "配置中必须设置 `$IDF_PATH。" -ForegroundColor Red
    exit 1
}
if (-not (Test-Path -LiteralPath $IDF_PATH)) {
    Write-Host "IDF_PATH 不存在: $IDF_PATH" -ForegroundColor Red
    exit 1
}

if (-not $ProjectDir) {
    $ProjectDir = $ScriptRoot
}

if ($IDF_TOOLS_PATH) { $env:IDF_TOOLS_PATH = $IDF_TOOLS_PATH }
if ($IDF_PYTHON_ENV_PATH) { $env:IDF_PYTHON_ENV_PATH = $IDF_PYTHON_ENV_PATH }
if (-not $env:IDF_PATH) { $env:IDF_PATH = $IDF_PATH }

function Normalize-FullPath {
    param([string]$Path)
    try {
        return [IO.Path]::GetFullPath($Path.TrimEnd('\', '/'))
    }
    catch {
        return $Path
    }
}

function Import-EimActivation {
    param(
        [string]$IdfPath,
        [string]$ToolsPath
    )

    $candidates = @()
    if ($ToolsPath) {
        $candidates += (Join-Path $ToolsPath "eim_idf.json")
    }
    $candidates += "C:\Espressif\tools\eim_idf.json"

    $eimJson = $null
    foreach ($p in $candidates) {
        if ($p -and (Test-Path -LiteralPath $p)) {
            $eimJson = $p
            break
        }
    }
    if (-not $eimJson) { return $false }

    try {
        $doc = Get-Content -LiteralPath $eimJson -Raw -Encoding UTF8 | ConvertFrom-Json
    }
    catch {
        return $false
    }

    $target = Normalize-FullPath $IdfPath
    foreach ($inst in $doc.idfInstalled) {
        if (-not $inst.path) { continue }
        $p = Normalize-FullPath $inst.path
        if ($p -ne $target) { continue }
        $act = $inst.activationScript
        if (-not $act) { continue }
        if (-not (Test-Path -LiteralPath $act)) { continue }
        Write-Host "使用 EIM 激活脚本: $act" -ForegroundColor Cyan
        . $act
        return $true
    }
    return $false
}

# 某些终端/工具会注入 MSYS/MINGW 环境变量，idf.py 会因此拒绝运行。
# 必须在激活 ESP-IDF 之前彻底清理，否则 idf_tools.py 会直接报错退出。
$msysVars = @(
    "MSYSTEM", "MINGW_PREFIX", "MINGW_CHOST", "MSYSTEM_CARCH", "MSYSTEM_CHOST",
    "MSYSTEM_PREFIX", "MINGW_PACKAGE_PREFIX", "MSYS2_PATH_TYPE"
)
$cleared = @()
foreach ($name in $msysVars) {
    $before = [Environment]::GetEnvironmentVariable($name, "Process")
    if ([string]::IsNullOrEmpty($before)) { continue }
    Remove-Item -Path ("Env:" + $name) -ErrorAction SilentlyContinue
    $after = [Environment]::GetEnvironmentVariable($name, "Process")
    if ([string]::IsNullOrEmpty($after)) { $cleared += $name }
}
# 同时从 PATH 中移除任何包含 msys 或 mingw 的路径（大小写不敏感）
if ($env:Path) {
    $originalPaths = $env:Path -split [IO.Path]::PathSeparator
    $cleanPaths = $originalPaths | Where-Object { $_ -notmatch '(?i)(msys|mingw)' }
    if ($cleanPaths.Count -lt $originalPaths.Count) {
        $env:Path = $cleanPaths -join [IO.Path]::PathSeparator
        $cleared += ("PATH中移除了" + ($originalPaths.Count - $cleanPaths.Count) + "个MSys/Mingw路径")
    }
}
if ($cleared.Count -gt 0) {
    Write-Host ("已清理 MSys/Mingw 环境: " + ($cleared -join ", ")) -ForegroundColor DarkGray
}

Write-Host "正在激活 ESP-IDF..." -ForegroundColor Cyan
$usedEim = Import-EimActivation -IdfPath $IDF_PATH -ToolsPath $env:IDF_TOOLS_PATH

if (-not $usedEim) {
    if ($IDF_PYTHON_ENV_PATH) {
        $venvScripts = Join-Path $IDF_PYTHON_ENV_PATH "Scripts"
        if (Test-Path -LiteralPath $venvScripts) {
            $env:Path = $venvScripts + [IO.Path]::PathSeparator + $env:Path
        }
    }
    $exportPs1 = Join-Path $IDF_PATH "export.ps1"
    if (-not (Test-Path -LiteralPath $exportPs1)) {
        Write-Host "未找到 EIM 配置且缺少 export.ps1: $exportPs1" -ForegroundColor Red
        exit 1
    }
    Write-Host "使用 export.ps1（未匹配到 eim_idf.json 中的本机 IDF_PATH）" -ForegroundColor Yellow
    . $exportPs1
}

if (-not (Get-Command idf.py -ErrorAction SilentlyContinue)) {
    Write-Host "未找到 idf.py。请检查 IDF_PATH 或 EIM 安装。" -ForegroundColor Red
    exit 1
}

Write-Host "工程目录: $ProjectDir" -ForegroundColor Cyan
Set-Location -LiteralPath $ProjectDir

if ($ActivateOnly) {
    Write-Host "ESP-IDF 环境已就绪，工程目录已切换。可直接使用 idf.py、menuconfig 等。" -ForegroundColor Green
    Write-Host "提示：用 -File 单独启动脚本时，进程退出后环境不会保留；交互式请点源本脚本，或把激活与命令写在同一条 -Command 里。" -ForegroundColor DarkGray
    Write-Host "  点源示例:  . `"$PSCommandPath`" -ActivateOnly" -ForegroundColor DarkGray
    Write-Host "  详见: Get-Help $PSCommandPath -Full （-ActivateOnly 用 return 以便同一条 -Command 里可继续执行 idf.py）" -ForegroundColor DarkGray
    return
}

Write-Host "idf.py build" -ForegroundColor Yellow
idf.py build
if ($LASTEXITCODE -ne 0) {
    Write-Host "编译失败，退出码: $LASTEXITCODE" -ForegroundColor Red
    exit $LASTEXITCODE
}
Write-Host "编译完成。" -ForegroundColor Green
