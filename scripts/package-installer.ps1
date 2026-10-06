param([string]$Configuration = 'Release', [string]$OutputDirectory = '', [switch]$SkipBuild)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$buildRoot = Join-Path $projectRoot "build/x64/$Configuration"
if (!$OutputDirectory) { $OutputDirectory = Join-Path $projectRoot 'results/distribution' }
$outputRoot = [IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Path $outputRoot -Force | Out-Null
if (!$SkipBuild) {
    & (Join-Path $projectRoot 'build.ps1') -Architecture x64 -Configuration $Configuration
    if ($LASTEXITCODE) { throw 'Native engine build failed' }
    & cmake --build $buildRoot --config $Configuration --target cod4x1khz_server_manager cod4x1khz_network --parallel 8
    if ($LASTEXITCODE) { throw 'Desktop application build failed' }
}
$transactionRoot = Join-Path $outputRoot ('payload-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $transactionRoot | Out-Null
$entries = [Collections.Generic.List[object]]::new()
function Add-PayloadFile([string]$Source, [string]$Relative) {
    $sourcePath = [IO.Path]::GetFullPath($Source)
    if (!(Test-Path -LiteralPath $sourcePath -PathType Leaf)) { throw "Missing package input: $sourcePath" }
    if (($Relative -match '(^|/)(main|zone)/.*\.(iwd|ff)$' -and $Relative -ne 'runtime/main/zz_cod4x1khz_bots.iwd') -or $Relative -match '(bench|regression|benchmark)' -or $Relative.Contains('..')) { throw "Forbidden package input: $Relative" }
    if ($Relative -match '\.(exe|dll)$') {
        $packageBytes = [IO.File]::ReadAllBytes($sourcePath)
        if ([Text.Encoding]::ASCII.GetString($packageBytes).Contains('local-khz-benchmark') -or
            [Text.Encoding]::Unicode.GetString($packageBytes).Contains('local-khz-benchmark')) {
            throw "Benchmark RCON credential is compiled into package input: $Relative"
        }
        $packageBytes = $null
        $binaryStream = [IO.File]::OpenRead($sourcePath)
        $binaryReader = [IO.BinaryReader]::new($binaryStream)
        try {
            if ($binaryReader.ReadUInt16() -ne 0x5a4d) { throw "Not a Windows PE image: $Relative" }
            $binaryStream.Position = 0x3c
            $peOffset = $binaryReader.ReadUInt32()
            if ($peOffset -gt $binaryStream.Length - 26) { throw "Invalid PE offset: $Relative" }
            $binaryStream.Position = $peOffset
            if ($binaryReader.ReadUInt32() -ne 0x4550 -or $binaryReader.ReadUInt16() -ne 0x8664) { throw "Package requires AMD64 images: $Relative" }
        } finally { $binaryReader.Dispose(); $binaryStream.Dispose() }
    }
    $name = 'file' + $entries.Count.ToString('D5')
    Copy-Item -LiteralPath $sourcePath -Destination (Join-Path $transactionRoot $name)
    $entries.Add([pscustomobject]@{ file = $name; relative = $Relative; sha256 = (Get-FileHash -LiteralPath $sourcePath -Algorithm SHA256).Hash.ToLowerInvariant(); bytes = (Get-Item -LiteralPath $sourcePath).Length })
}
$nativeRoot = Join-Path $projectRoot 'runtime/x64'
Add-PayloadFile (Join-Path $nativeRoot 'cod4x1khz.exe') 'cod4x1khz.exe'
Add-PayloadFile (Join-Path $buildRoot "desktop/$Configuration/cod4x1khz_server_manager.exe") 'cod4x1khz_server_manager.exe'
Add-PayloadFile (Join-Path $nativeRoot 'cod4x1khz_server.exe') 'runtime/x64/cod4x1khz_server.exe'
Add-PayloadFile (Join-Path $nativeRoot 'cod4x1khz_update.exe') 'runtime/x64/cod4x1khz_update.exe'
Add-PayloadFile (Join-Path $buildRoot "desktop/$Configuration/cod4x1khz_network.exe") 'runtime/tools/x64/cod4x1khz_network.exe'
foreach ($name in @('avcodec-62.dll','avformat-62.dll','avutil-60.dll','swresample-6.dll','swscale-9.dll','D3DX9_43.dll')) { Add-PayloadFile (Join-Path $nativeRoot $name) $name }
Add-PayloadFile (Join-Path $nativeRoot 'D3DX9_43.dll') 'runtime/x64/D3DX9_43.dll'
Add-PayloadFile (Join-Path $projectRoot 'config/update-policy.txt') 'config/update-policy.txt'
Add-PayloadFile (Join-Path $projectRoot 'LICENSE') 'notices/project-LICENSE.txt'
Add-PayloadFile (Join-Path $projectRoot 'engine/LICENSE') 'notices/engine-LICENSE.txt'
Add-PayloadFile (Join-Path $projectRoot '.deps/dxsdk/9.29.952.8/LICENSE.txt') 'notices/Microsoft-D3DX-LICENSE.txt'
$productionBots = Join-Path $transactionRoot 'production-bots.iwd'
& (Join-Path $PSScriptRoot 'package-bots.ps1') -Production -OutputPath $productionBots | Out-Null
Add-PayloadFile $productionBots 'runtime/main/zz_cod4x1khz_bots.iwd'
Add-PayloadFile (Join-Path $projectRoot 'upstream/bot_warfare/README.md') 'notices/Bot-Warfare-README.md'
$videoRoot = Join-Path $projectRoot '.deps/ffmpeg/ffmpeg-n8.1.3-9-g29e619e767-win64-lgpl-shared-8.1'
Add-PayloadFile (Join-Path $videoRoot 'LICENSE.txt') 'notices/ffmpeg/LICENSE.txt'
foreach ($file in Get-ChildItem -LiteralPath (Join-Path $videoRoot 'doc') -File) {
    if ($file.Name -match '(license|licensing|copying|legal|readme)' -and $file.Length -lt 1000000) { Add-PayloadFile $file.FullName ('notices/ffmpeg/' + $file.Name) }
}
$notices = @'
cod4x1khz distribution

Retail CoD4 assets are not included in this installer. They are copied from the user's own installed Steam game.
KisakCOD and cod4x1khz source/license notices are included alongside this file.
Bot Warfare source is pinned at 7b52b462e40fa68287b9e54602e52da83b17766c:
https://github.com/ineedbots/iw3_bot_warfare
FFmpeg shared LGPL build is pinned by scripts/prepare-native-video.ps1. Its original documentation and notices accompany this distribution.
Source and build instructions: https://github.com/JarvisUltronSlop/cod4x1khz/tree/vulkan-testing
Vulkan loader/graphics drivers are supplied by the user's graphics driver installation.
Microsoft D3DX9_43 runtime originates from Microsoft.DXSDK.D3DX 9.29.952.8.
'@
[IO.File]::WriteAllText((Join-Path $transactionRoot 'notices.txt'), $notices, [Text.UTF8Encoding]::new($false))
Add-PayloadFile (Join-Path $transactionRoot 'notices.txt') 'notices/THIRD-PARTY.txt'
$manifest = ($entries | ForEach-Object { "$($_.file)`t$($_.sha256)`t$($_.relative)" }) -join "`n"
[IO.File]::WriteAllText((Join-Path $transactionRoot 'manifest.tsv'), $manifest + "`n", [Text.UTF8Encoding]::new($false))
$cabinet = Join-Path $outputRoot 'cod4x1khz-payload.cab'
$directive = @('.OPTION EXPLICIT','.Set Cabinet=ON','.Set Compress=ON','.Set CompressionType=MSZIP','.Set MaxDiskSize=0',('.Set DiskDirectoryTemplate="' + $outputRoot + '"'),'.Set CabinetNameTemplate=cod4x1khz-payload.cab')
$directive += '.Set RptFileName="' + (Join-Path $transactionRoot 'setup.rpt') + '"'
$directive += '.Set InfFileName="' + (Join-Path $transactionRoot 'setup.inf') + '"'
$directive += '"' + (Join-Path $transactionRoot 'manifest.tsv') + '" manifest.tsv'
$directive += $entries | ForEach-Object { '"' + (Join-Path $transactionRoot $_.file) + '" ' + $_.file }
$directiveFile = Join-Path $transactionRoot 'payload.ddf'
[IO.File]::WriteAllLines($directiveFile, $directive, [Text.UTF8Encoding]::new($false))
& makecab.exe /F $directiveFile | Out-File (Join-Path $outputRoot 'cabinet-build.log')
if ($LASTEXITCODE) { throw 'Embedded cabinet build failed' }
$cabinetOption = '-DKHZ_INSTALLER_PAYLOAD=' + $cabinet.Replace('\','/')
& cmake -S $projectRoot -B $buildRoot $cabinetOption
if ($LASTEXITCODE) { throw 'Installer configure failed' }
& cmake --build $buildRoot --config $Configuration --target cod4x1khz_setup --parallel 8
if ($LASTEXITCODE) { throw 'Installer build failed' }
$installer = Join-Path $outputRoot 'cod4x1khz_setup.exe'
Copy-Item -LiteralPath (Join-Path $buildRoot "desktop/$Configuration/cod4x1khz_setup.exe") -Destination $installer -Force
$report = [ordered]@{ installer = $installer; sha256 = (Get-FileHash -LiteralPath $installer -Algorithm SHA256).Hash.ToLowerInvariant(); retail_assets_embedded = $false; benchmark_tools_embedded = $false; benchmark_rcon_password_embedded = $false; package_files = $entries; binary_abi = 'AMD64'; github_release_created = $false }
$report | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $outputRoot 'package-report.json') -Encoding utf8
Write-Output $installer
