$ErrorActionPreference = 'Stop'
$nativeVideoRoot = Join-Path $PSScriptRoot '../.deps/ffmpeg'
$nativeVideoArchive = Join-Path $nativeVideoRoot 'ffmpeg-monthly-shared.zip'
$nativeVideoExpected = '3e47bda1607740550141e37c0e49d1e5182b34699f15adfd137ee266d346811a'
New-Item -ItemType Directory -Path $nativeVideoRoot -Force | Out-Null
if (!(Test-Path -LiteralPath $nativeVideoArchive)) {
    Invoke-WebRequest 'https://github.com/BtbN/FFmpeg-Builds/releases/download/autobuild-2026-09-30-13-08/ffmpeg-n8.1.3-9-g29e619e767-win64-lgpl-shared-8.1.zip' -OutFile $nativeVideoArchive
}
if ((Get-FileHash -LiteralPath $nativeVideoArchive -Algorithm SHA256).Hash.ToLowerInvariant() -ne $nativeVideoExpected) {
    throw 'Native video archive does not match the pinned FFmpeg release hash'
}
# BtbN retains the last build of each month for two years.
$nativeVideoPackage = Join-Path $nativeVideoRoot 'ffmpeg-n8.1.3-9-g29e619e767-win64-lgpl-shared-8.1'
if (!(Test-Path -LiteralPath (Join-Path $nativeVideoPackage 'include/libavcodec/avcodec.h'))) {
    Expand-Archive -LiteralPath $nativeVideoArchive -DestinationPath $nativeVideoRoot -Force
}
(Resolve-Path -LiteralPath $nativeVideoPackage).Path
