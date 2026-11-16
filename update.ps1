#Requires -Version 5.1
# Rebuild and reinstall the Windows thin client. Windows gets only the thin
# client; it reaches a remote aimee-server (AIMEE_SERVER_URL / --server).
$ErrorActionPreference = "Stop"

try {
    $repoRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
    $buildDir = Join-Path $repoRoot "build"
    $installRoot = Join-Path $env:LOCALAPPDATA "aimee"
    $binDir = Join-Path $installRoot "bin"

    Write-Host "> Building the aimee thin client with CMake..." -ForegroundColor Green
    Push-Location $repoRoot
    try {
        & cmake -B $buildDir -S $repoRoot
        & cmake --build $buildDir --config Release --target aimee
    }
    finally {
        Pop-Location
    }

    $cliCandidate = @(
        (Join-Path (Join-Path $buildDir "Release") "aimee.exe"),
        (Join-Path $buildDir "aimee.exe")
    ) | Where-Object { Test-Path $_ } | Select-Object -First 1
    if (-not $cliCandidate) {
        throw "Built binary not found: aimee.exe"
    }

    Write-Host "> Updating $binDir\aimee.exe..." -ForegroundColor Green
    New-Item -ItemType Directory -Force -Path $binDir | Out-Null
    Copy-Item $cliCandidate (Join-Path $binDir "aimee.exe") -Force

    Write-Host ""
    Write-Host "Update complete." -ForegroundColor Green
}
catch {
    Write-Host "Update failed: $($_.Exception.Message)" -ForegroundColor Red
    exit 1
}
