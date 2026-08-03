#requires -Version 7.0
[CmdletBinding()]
param(
    [switch]$Windows,
    [switch]$Linux,
    [switch]$Clean,
    [string]$Config = "Release",
    [string]$WslDistro = "Ubuntu-24.04",
    [string]$Generator = "Visual Studio 18 2026"
)

$ErrorActionPreference = "Stop"

# Force UTF-8 so CMake/MSBuild output renders correctly
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
$OutputEncoding = [System.Text.Encoding]::UTF8

$Root = $PSScriptRoot
$DistWin = Join-Path $Root "dist/windows"
$DistLin = Join-Path $Root "dist/linux"

if (-not ($Windows -or $Linux)) {
    $Windows = $true
    $Linux = $true
}

# Print a build step heading
function Write-Step([string]$Message) { Write-Host "`n=== $Message ===" -ForegroundColor Cyan }

# Print a successful build result
function Write-Ok([string]$Message) { Write-Host "  $Message" -ForegroundColor Green }

# Convert a Windows path to a WSL mount path
function ConvertTo-WslPath([string]$Path) {
    $full = (Resolve-Path -LiteralPath $Path).Path
    $drive = $full.Substring(0, 1).ToLower()
    $rest = $full.Substring(2) -replace '\\', '/'
    return "/mnt/$drive$rest"
}

if ($Clean) {
    Write-Step "Clean"
    foreach ($Directory in @("build", "dist")) {
        $Path = Join-Path $Root $Directory
        if (Test-Path -LiteralPath $Path) {
            Remove-Item -LiteralPath $Path -Recurse -Force
            Write-Ok "removed $Directory/"
        }
    }
    if ($Linux) {
        wsl.exe -d $WslDistro -e bash -lc "rm -rf ~/bv-build" | Out-Null
        Write-Ok "removed ~/bv-build (WSL)"
    }
}

# Configure and build the Windows plugin
function Build-Windows {
    Write-Step "Windows"
    $BuildDir = Join-Path $Root "build"
    cmake -B $BuildDir -G $Generator -A x64 -S $Root | Out-Host
    if ($LASTEXITCODE) { throw "cmake configure (windows) failed" }

    cmake --build $BuildDir --config $Config | Out-Host
    if ($LASTEXITCODE) { throw "cmake build (windows) failed" }

    $Package = Join-Path $BuildDir "package"
    if (-not (Test-Path -LiteralPath "$Package/addons/BotVision/bin/win64/BotVision.dll")) {
        throw "windows build produced no BotVision.dll"
    }
    Write-Ok "BotVision.dll built"
    return $Package
}

# Configure and build the Linux plugin through WSL
function Build-Linux {
    Write-Step "Linux (WSL: $WslDistro)"
    $SourceWsl = ConvertTo-WslPath $Root
    $Hl2Wsl = ConvertTo-WslPath $env:HL2SDKCS2
    $MmsWsl = ConvertTo-WslPath $env:MMSOURCE_DEV
    $ProtoExport = "unset CSGO_PROTO"
    if ($env:CSGO_PROTO) {
        $ProtoWsl = ConvertTo-WslPath $env:CSGO_PROTO
        $ProtoExport = "export CSGO_PROTO='$ProtoWsl'"
    }

    $Bash = @"
set -e
export HL2SDKCS2='$Hl2Wsl'
export MMSOURCE_DEV='$MmsWsl'
$ProtoExport
cmake -S '$SourceWsl' -B ~/bv-build -DCMAKE_BUILD_TYPE=$Config
cmake --build ~/bv-build -j`$(nproc)
test -f ~/bv-build/package/addons/BotVision/bin/linuxsteamrt64/BotVision.so
echo "BUILD_OK"
"@
    $Bash = $Bash -replace "`r`n", "`n"
    wsl.exe -d $WslDistro -e bash -lc $Bash | Out-Host
    if ($LASTEXITCODE) { throw "WSL linux build failed" }

    $Stage = Join-Path $Root "build/linux-package"
    if (Test-Path -LiteralPath $Stage) { Remove-Item -LiteralPath $Stage -Recurse -Force }
    New-Item -ItemType Directory -Path $Stage -Force | Out-Null
    $StageWsl = ConvertTo-WslPath $Stage
    wsl.exe -d $WslDistro -e bash -lc "cp -r ~/bv-build/package/. '$StageWsl/'" | Out-Host
    if ($LASTEXITCODE) { throw "copying WSL package out failed" }
    Write-Ok "BotVision.so built and staged to build/linux-package/"
    return $Stage
}

# Assemble one platform package
function Build-Dist([string]$Package, [string]$Destination) {
    if (Test-Path -LiteralPath $Destination) {
        Remove-Item -LiteralPath $Destination -Recurse -Force
    }
    New-Item -ItemType Directory -Path $Destination -Force | Out-Null
    Copy-Item -LiteralPath (Join-Path $Package "addons") -Destination $Destination -Recurse -Force
    Write-Ok "assembled $((Resolve-Path -LiteralPath $Destination).Path)"
}

$WindowsPackage = $null
$LinuxPackage = $null
if ($Windows) { $WindowsPackage = Build-Windows }
if ($Linux) { $LinuxPackage = Build-Linux }

Write-Step "Dist"
if ($Windows) { Build-Dist $WindowsPackage $DistWin }
if ($Linux) { Build-Dist $LinuxPackage $DistLin }

Write-Step "Done"
if ($Windows) { Write-Ok "Windows -> dist/windows/" }
if ($Linux) { Write-Ok "Linux   -> dist/linux/" }
