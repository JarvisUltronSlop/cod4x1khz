param([string]$GameDirectory = 'D:\SteamLibrary\steamapps\common\Call of Duty 4',
    [ValidateSet('x64','Win32')][string]$Architecture = 'x64')
$ErrorActionPreference = 'Stop'
$projectRoot = $PSScriptRoot
# Retail files must come from the developer's own installed Steam copy.
foreach ($required in @('iw3mp.exe', 'localization.txt')) {
    if (!(Test-Path -LiteralPath (Join-Path $GameDirectory $required) -PathType Leaf)) {
        throw "Choose your installed Steam Call of Duty 4 (2007) folder; missing $required"
    }
}
foreach ($required in @('main', 'zone')) {
    if (!(Test-Path -LiteralPath (Join-Path $GameDirectory $required) -PathType Container)) {
        throw "Choose your installed Steam Call of Duty 4 (2007) folder; missing $required"
    }
}
. (Join-Path $projectRoot 'scripts/source-revision.ps1')
$pins = Get-Content -LiteralPath (Join-Path $projectRoot 'upstream-lock.json') -Raw | ConvertFrom-Json
foreach ($entry in @(@('engine', $pins.KisakCOD), @('upstream/cod4x_server', $pins.CoD4x_Server), @('upstream/cod4x_client', $pins.CoD4x_Client), @('upstream/bot_warfare', $pins.Bot_Warfare))) {
    $checkoutPath = Join-Path $projectRoot $entry[0]
    if (!(Test-Path -LiteralPath $checkoutPath)) {
        throw "Missing source snapshot: $checkoutPath. Restore it from this repository; upstream binary bundles are not required for the native x64 build."
    }
    $actualRevision = Get-ProjectSourceRevision -ProjectRoot $projectRoot -RelativePath $entry[0]
    if ($actualRevision -ne $entry[1].commit) { throw "Unexpected source revision in $checkoutPath; preserve local edits and inspect before changing the pin" }
}
$sdkRoot = Join-Path $projectRoot '.deps/dxsdk/9.29.952.8'
if (!(Test-Path -LiteralPath (Join-Path $sdkRoot 'build/native/include/d3dx9.h'))) {
    New-Item -ItemType Directory -Force -Path $sdkRoot | Out-Null
    $archive = Join-Path $sdkRoot 'dxsdk.zip'
    Invoke-WebRequest -Uri 'https://api.nuget.org/v3-flatcontainer/microsoft.dxsdk.d3dx/9.29.952.8/microsoft.dxsdk.d3dx.9.29.952.8.nupkg' -OutFile $archive
    if ((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant() -ne $pins.DirectX.sha256) { throw 'DirectX package checksum mismatch' }
    Expand-Archive -LiteralPath $archive -DestinationPath $sdkRoot -Force
}
$runtimeRoot = Join-Path $projectRoot 'runtime'
New-Item -ItemType Directory -Force -Path $runtimeRoot | Out-Null
foreach ($directory in @('main', 'zone')) {
    if (!(Test-Path -LiteralPath (Join-Path $runtimeRoot $directory))) {
        if (!(Test-Path -LiteralPath (Join-Path $GameDirectory $directory))) { throw "Missing game data: $GameDirectory/$directory" }
        Copy-Item -LiteralPath (Join-Path $GameDirectory $directory) -Destination $runtimeRoot -Recurse
    }
}
Copy-Item -LiteralPath (Join-Path $GameDirectory 'localization.txt') -Destination $runtimeRoot -Force
if ($Architecture -eq 'Win32') {
    # The historical Win32 runtime is optional and never supplied by Git.
    foreach ($dependency in @('mss32.dll', 'binkw32.dll', 'steam_api.dll')) {
        $source = Join-Path $GameDirectory $dependency
        if (!(Test-Path -LiteralPath $source -PathType Leaf)) {
            throw "The optional Win32 runtime requires $dependency from your owned game. Use the native x64 build for release 0.2."
        }
        Copy-Item -LiteralPath $source -Destination $runtimeRoot -Force
        Copy-Item -LiteralPath $source -Destination $projectRoot -Force
    }
    $miles = Join-Path $GameDirectory 'miles'
    if (Test-Path -LiteralPath $miles -PathType Container) {
        Copy-Item -LiteralPath $miles -Destination $runtimeRoot -Recurse -Force
    }
    $d3dx = Join-Path $sdkRoot 'build/native/release/bin/x86/D3DX9_43.dll'
    Copy-Item -LiteralPath $d3dx -Destination $runtimeRoot -Force
    Copy-Item -LiteralPath $d3dx -Destination $projectRoot -Force
}
Copy-Item -Path (Join-Path $projectRoot 'config/*.cfg') -Destination (Join-Path $runtimeRoot 'main') -Force
# The executable creates a fresh stock interactive profile in runtime/interactive.
# Benchmark profiles are created under runtime/benchmarks and never share its home.
Write-Output "Runtime prepared at $runtimeRoot"
& (Join-Path $projectRoot 'scripts/prepare-vulkan.ps1')
