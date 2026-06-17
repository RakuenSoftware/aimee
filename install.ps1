#Requires -Version 5.1
# $Portable is accepted for backwards compatibility (it previously skipped
# Windows service registration); the Windows install is now thin-client only and
# registers no services, so the switch is a no-op.
param([switch]$Portable)
$ErrorActionPreference = "Stop"

# aimee Windows installer — THIN CLIENT ONLY.
#
# On Windows aimee runs as the thin client: a single `aimee.exe` that talks to a
# remote aimee-server over the /v1 HTTP API. The server (aimee-server) and the
# knowledge base (aimee-kb) run in Docker or on a Linux/macOS host, not on
# Windows. This script builds that client with CMake's thin-client profile and
# installs it to %LOCALAPPDATA%\aimee\bin; it does not build or run any services.
# (Prefer no build at all? Download the prebuilt aimee-windows-x86_64.exe from a
# GitHub release and put it on your PATH instead.)

try {
    Write-Host "> Checking prerequisites..." -ForegroundColor Green

    $git = Get-Command git -ErrorAction SilentlyContinue
    if (-not $git) {
        throw "Git is not installed or not available on PATH."
    }

    $cl = Get-Command cl.exe -ErrorAction SilentlyContinue
    $gcc = Get-Command gcc -ErrorAction SilentlyContinue
    if (-not $cl -and -not $gcc) {
        throw "No supported C compiler found. Install Visual Studio Build Tools (cl.exe) or MinGW gcc."
    }

    $cmake = Get-Command cmake -ErrorAction SilentlyContinue
    if (-not $cmake) {
        throw "cmake is not installed or not available on PATH."
    }

    $repoRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
    $buildDir = Join-Path $repoRoot "build"
    $installRoot = Join-Path $env:LOCALAPPDATA "aimee"
    $binDir = Join-Path $installRoot "bin"

    Write-Host "> Building the aimee thin client with CMake..." -ForegroundColor Green
    Push-Location $repoRoot
    try {
        # Thin-client profile: only aimee.exe (no server/kb/gateway/webchat, so no
        # libpq / Go / PAM). TLS uses Schannel (Windows cert store), so https://
        # remote aimee-servers work with no OpenSSL. Matches the CI and release
        # Windows builds.
        $cfgArgs = @(
            "-B", $buildDir,
            "-DAIMEE_THIN_CLIENT=ON",
            "-DAIMEE_LEAN=ON",
            "-DWITH_PAM=OFF",
            "-DWITH_LIBSECRET=OFF",
            "-DWITH_UI=OFF",
            "-DWITH_TLS=ON"
        )
        # MinGW gcc needs an explicit Makefiles generator; MSVC uses its default.
        if ($gcc -and -not $cl) {
            $cfgArgs += @("-G", "MinGW Makefiles")
        }
        & cmake @cfgArgs
        & cmake --build $buildDir --config Release --target aimee
    }
    finally {
        Pop-Location
    }

    $releaseDir = Join-Path $buildDir "Release"
    $cliCandidate = @(
        (Join-Path $releaseDir "aimee.exe"),
        (Join-Path $buildDir "aimee.exe")
    ) | Where-Object { Test-Path $_ } | Select-Object -First 1

    if (-not $cliCandidate) {
        throw "Built binary not found: aimee.exe"
    }

    Write-Host "> Installing aimee.exe to $binDir..." -ForegroundColor Green
    New-Item -ItemType Directory -Force -Path $binDir | Out-Null
    Copy-Item $cliCandidate (Join-Path $binDir "aimee.exe") -Force

    Write-Host "> Updating user PATH..." -ForegroundColor Green
    $userPath = [Environment]::GetEnvironmentVariable("Path", "User")
    $pathEntries = @()
    if ($userPath) {
        $pathEntries = $userPath -split ';' | Where-Object { $_ -and $_.Trim() -ne '' }
    }
    $binDirNormalized = [System.IO.Path]::GetFullPath($binDir)
    $alreadyPresent = $false
    foreach ($entry in $pathEntries) {
        try {
            if ([System.StringComparer]::OrdinalIgnoreCase.Equals([System.IO.Path]::GetFullPath($entry), $binDirNormalized)) {
                $alreadyPresent = $true
                break
            }
        }
        catch {
            if ([System.StringComparer]::OrdinalIgnoreCase.Equals($entry, $binDirNormalized)) {
                $alreadyPresent = $true
                break
            }
        }
    }
    if (-not $alreadyPresent) {
        $newPath = if ($userPath -and $userPath.Trim()) { "$userPath;$binDir" } else { $binDir }
        [Environment]::SetEnvironmentVariable("Path", $newPath, "User")
        $env:Path = "$env:Path;$binDir"
        Write-Host "  Added $binDir to your user PATH." -ForegroundColor Yellow
    }
    else {
        Write-Host "  User PATH already includes $binDir." -ForegroundColor Yellow
    }

    Write-Host ""
    Write-Host "Thin client installed." -ForegroundColor Green
    Write-Host "Next steps:" -ForegroundColor Green
    Write-Host "  1. Open a new PowerShell or Command Prompt so PATH changes take effect."
    Write-Host "  2. Point aimee at your aimee-server (running in Docker or on Linux/macOS):"
    Write-Host "       aimee remote set http://YOUR_SERVER:8740 YOUR_BEARER_TOKEN"
    Write-Host "       aimee remote status"
    Write-Host "     (or set AIMEE_SERVER_URL / AIMEE_SERVER_TOKEN, or pass --server per command)."
    Write-Host "  3. Run .\configure-hooks.ps1 to configure Claude, Codex, Gemini, or Copilot hooks."
}
catch {
    Write-Host "Install failed: $($_.Exception.Message)" -ForegroundColor Red
    exit 1
}
