#requires -Version 7.0
[CmdletBinding()]
param(
    [switch]$Clean,
    [string]$Config = "Release",
    [string]$Generator = "Visual Studio 18 2026"
)

$ErrorActionPreference = "Stop"

# Force UTF-8 so CMake and MSBuild output renders correctly
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
$OutputEncoding = [System.Text.Encoding]::UTF8

$Root = $PSScriptRoot
$BuildDir = Join-Path $Root "build"
$DistDir = Join-Path $Root "dist/windows"

# Print a build step heading
function Write-Step([string]$Message) {
    Write-Host "`n=== $Message ===" -ForegroundColor Cyan
}

# Print a successful build result
function Write-Ok([string]$Message) {
    Write-Host "  $Message" -ForegroundColor Green
}

# Configure and build the Windows plugin
function Build-Windows {
    Write-Step "Windows"
    cmake -B $BuildDir -G $Generator -A x64 -S $Root | Out-Host
    if ($LASTEXITCODE) { throw "cmake configure failed" }

    cmake --build $BuildDir --config $Config | Out-Host
    if ($LASTEXITCODE) { throw "cmake build failed" }

    $Plugin = Join-Path $BuildDir "$Config/BotVision.dll"
    if (-not (Test-Path -LiteralPath $Plugin)) {
        throw "build produced no BotVision.dll"
    }

    Write-Ok "BotVision.dll built"
    return $Plugin
}

# Assemble the deployable Windows package
function Build-Dist([string]$Plugin) {
    Write-Step "Dist"
    if (Test-Path -LiteralPath $DistDir) {
        Remove-Item -LiteralPath $DistDir -Recurse -Force
    }

    New-Item -ItemType Directory -Path $DistDir -Force | Out-Null
    Copy-Item -LiteralPath (Join-Path $Root "configs/addons") -Destination $DistDir -Recurse -Force

    $BinDir = Join-Path $DistDir "addons/BotVision/bin/win64"
    New-Item -ItemType Directory -Path $BinDir -Force | Out-Null
    Copy-Item -LiteralPath $Plugin -Destination $BinDir -Force
    Write-Ok "assembled $DistDir"
}

if ($Clean) {
    Write-Step "Clean"
    foreach ($Directory in @($BuildDir, (Join-Path $Root "dist"))) {
        if (Test-Path -LiteralPath $Directory) {
            Remove-Item -LiteralPath $Directory -Recurse -Force
            Write-Ok "removed $Directory"
        }
    }
}

$Plugin = Build-Windows
Build-Dist $Plugin

Write-Step "Done"
Write-Ok "Windows -> dist/windows/"
