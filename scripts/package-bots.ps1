$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$sourceRoot = Join-Path $projectRoot 'upstream/bot_warfare'
$runtimeRoot = Join-Path $projectRoot 'runtime'
$pins = Get-Content -LiteralPath (Join-Path $projectRoot 'upstream-lock.json') -Raw | ConvertFrom-Json
. (Join-Path $PSScriptRoot 'source-revision.ps1')
$revision = Get-ProjectSourceRevision -ProjectRoot $projectRoot -RelativePath 'upstream/bot_warfare'
if ($revision -ne $pins.Bot_Warfare.commit) { throw 'Bot Warfare checkout does not match its locked revision' }
Add-Type -AssemblyName System.IO.Compression
$files = [System.Collections.Generic.SortedDictionary[string,string]]::new([System.StringComparer]::Ordinal)
foreach ($directory in @('maps/mp/bots','scripts','scriptdata')) {
    $directoryRoot = Join-Path $sourceRoot $directory
    foreach ($file in Get-ChildItem -LiteralPath $directoryRoot -File -Recurse) {
        $relative = [System.IO.Path]::GetRelativePath($sourceRoot,$file.FullName).Replace('\','/')
        $files[$relative] = $file.FullName
    }
}
# Explicit native bootstrap replaces the upstream callback GSX autoloader.
$overlayRoot = Join-Path $projectRoot 'config/bots'
foreach ($file in Get-ChildItem -LiteralPath $overlayRoot -File -Recurse) {
    $relative = [System.IO.Path]::GetRelativePath($overlayRoot,$file.FullName).Replace('\','/')
    $files[$relative] = $file.FullName
}
$files['notices/bot-warfare-README.md'] = Join-Path $sourceRoot 'README.md'
$files['notices/upstream-lock.json'] = Join-Path $projectRoot 'upstream-lock.json'
# Compiles/runs only with +set khz_plugin_tests 1 after loading its rebuilt DLL.
$files['scripts/khz_plugin_fixture.gsc'] = Join-Path $projectRoot 'sdk/tests/plugin_fixture.gsc'
$destination = Join-Path $runtimeRoot 'main/zz_cod4x1khz_bots.iwd'
$temporary = "$destination.tmp"
$stream = [System.IO.File]::Create($temporary)
$archive = [System.IO.Compression.ZipArchive]::new($stream,[System.IO.Compression.ZipArchiveMode]::Create)
try {
    foreach ($entry in $files.GetEnumerator()) {
        $zipEntry = $archive.CreateEntry($entry.Key,[System.IO.Compression.CompressionLevel]::Optimal)
        $zipEntry.LastWriteTime = [System.DateTimeOffset]::new(2020,1,1,0,0,0,[TimeSpan]::Zero)
        $inputStream = [System.IO.File]::OpenRead($entry.Value)
        $outputStream = $zipEntry.Open()
        try { $inputStream.CopyTo($outputStream) } finally { $outputStream.Dispose(); $inputStream.Dispose() }
    }
} finally { $archive.Dispose(); $stream.Dispose() }
Move-Item -LiteralPath $temporary -Destination $destination -Force
$noticeRoot = Join-Path $runtimeRoot 'notices'
New-Item -ItemType Directory -Force -Path $noticeRoot | Out-Null
Copy-Item -LiteralPath (Join-Path $sourceRoot 'README.md') -Destination (Join-Path $noticeRoot 'Bot-Warfare-README.md') -Force
[pscustomobject]@{ Package=$destination; Files=$files.Count; Revision=$revision.Trim(); SHA256=(Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash.ToLowerInvariant() }
