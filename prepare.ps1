param([string]$GameDirectory = 'D:\SteamLibrary\steamapps\common\Call of Duty 4')
$ErrorActionPreference = 'Stop'
$projectRoot = $PSScriptRoot
. (Join-Path $projectRoot 'scripts/source-revision.ps1')
$pins = Get-Content -LiteralPath (Join-Path $projectRoot 'upstream-lock.json') -Raw | ConvertFrom-Json
foreach ($entry in @(@('engine', $pins.KisakCOD), @('upstream/cod4x_server', $pins.CoD4x_Server), @('upstream/cod4x_client', $pins.CoD4x_Client), @('upstream/bot_warfare', $pins.Bot_Warfare))) {
    $checkoutPath = Join-Path $projectRoot $entry[0]
    if (!(Test-Path -LiteralPath $checkoutPath)) {
        & git clone $entry[1].url $checkoutPath
        if ($LASTEXITCODE) { throw 'Source download failed' }
        & git -C $checkoutPath checkout --detach $entry[1].commit
        if ($LASTEXITCODE) { throw 'Pinned checkout failed' }
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
foreach ($source in @('engine/deps/msslib/dlls/*', 'engine/deps/binklib/binkw32.dll', 'engine/deps/steamsdk/steam_api.dll', '.deps/dxsdk/9.29.952.8/build/native/release/bin/x86/*.dll')) {
    Copy-Item -Path (Join-Path $projectRoot $source) -Destination $runtimeRoot -Recurse -Force
}
Copy-Item -Path (Join-Path $projectRoot 'config/*.cfg') -Destination (Join-Path $runtimeRoot 'main') -Force
# The executable creates a fresh stock interactive profile in runtime/interactive.
# Benchmark profiles are created under runtime/benchmarks and never share its home.
foreach ($dependency in @('mss32.dll','D3DX9_43.dll','binkw32.dll','steam_api.dll')) {
    Copy-Item -LiteralPath (Join-Path $runtimeRoot $dependency) -Destination $projectRoot -Force
}
Write-Output "Runtime prepared at $runtimeRoot"
& (Join-Path $projectRoot 'scripts/prepare-vulkan.ps1')
