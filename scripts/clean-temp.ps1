[CmdletBinding(SupportsShouldProcess = $true)]
param(
    [switch]$CleanBuild,
    [switch]$CleanManagedComponents
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

# Script location: <repo>/scripts/clean-temp.ps1
$projectRoot = Split-Path -Parent $PSScriptRoot

# Safe default: remove temporary/cache artifacts only.
$targets = @(
    ".cache",
    "tmp",
    "sdkconfig.old",
    "build\log",
    "build\.cache",
    "build\CMakeFiles\CMakeScratch",
    "build\.bin_timestamp",
    "build\.ninja_log",
    "build\.ninja_deps"
)

if ($CleanBuild) {
    $targets += "build"
}

if ($CleanManagedComponents) {
    $targets += "managed_components"
}

$removed = 0
$skipped = 0

foreach ($relativePath in ($targets | Select-Object -Unique)) {
    $fullPath = Join-Path $projectRoot $relativePath

    if (-not (Test-Path -LiteralPath $fullPath)) {
        Write-Host "[skip] $relativePath (not found)"
        $skipped++
        continue
    }

    if ($PSCmdlet.ShouldProcess($fullPath, "Remove")) {
        Remove-Item -LiteralPath $fullPath -Recurse -Force
        Write-Host "[ok] Removed $relativePath"
        $removed++
    }
}

Write-Host ""
Write-Host "Cleanup complete. Removed: $removed, Skipped: $skipped"
Write-Host "Project root: $projectRoot"

if (-not $CleanBuild) {
    Write-Host "Tip: run with -CleanBuild to remove the entire build directory."
}
if (-not $CleanManagedComponents) {
    Write-Host "Tip: run with -CleanManagedComponents to force component re-download."
}
