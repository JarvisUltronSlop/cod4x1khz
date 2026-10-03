param([string]$SourceExecutable = 'D:\SteamLibrary\steamapps\common\Call of Duty 4\iw3mp.exe')
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$outputRoot = Join-Path $projectRoot '.deps/icon-source'
New-Item -ItemType Directory -Force -Path $outputRoot | Out-Null
Add-Type -AssemblyName System.Drawing
$icon = [System.Drawing.Icon]::ExtractAssociatedIcon($SourceExecutable)
if (!$icon) { throw 'Installed executable has no associated icon' }
try {
    $stream = [System.IO.File]::Create((Join-Path $outputRoot 'original-cod4.ico'))
    try { $icon.Save($stream) } finally { $stream.Dispose() }
    $bitmap = $icon.ToBitmap()
    try { $bitmap.Save((Join-Path $outputRoot 'original-cod4.png'), [System.Drawing.Imaging.ImageFormat]::Png) } finally { $bitmap.Dispose() }
} finally { $icon.Dispose() }
Get-FileHash -LiteralPath (Join-Path $outputRoot 'original-cod4.ico') -Algorithm SHA256
