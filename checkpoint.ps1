param()
$ErrorActionPreference = 'Stop'
$projectRoot = (Resolve-Path -LiteralPath $PSScriptRoot).Path
$tag = (Get-Date -Format 'yyyyMMdd-HHmmss') + '-' + [guid]::NewGuid().ToString('N').Substring(0,8)
$checkpointRoot = Join-Path $projectRoot "results/source-$tag"
$archivePath = "$checkpointRoot.zip"
New-Item -ItemType Directory -Path $checkpointRoot | Out-Null
$patchPath = Join-Path $checkpointRoot 'engine-working.patch'
& git -C (Join-Path $projectRoot 'engine') diff HEAD --binary "--output=$patchPath"
if ($LASTEXITCODE) { throw 'Could not save engine modifications' }
& git -C (Join-Path $projectRoot 'engine') apply --check --reverse $patchPath
if ($LASTEXITCODE) { throw 'Saved engine patch does not match the current source' }
$revision = & git -C (Join-Path $projectRoot 'engine') rev-parse HEAD
if ($LASTEXITCODE) { throw 'Could not read the engine base revision' }
$binaryHashes = @{}
foreach ($name in 'cod4x1khz.exe','cod4x1khz_server.exe','cod4x1khz_bench.exe') {
    $binaryPath = Join-Path $projectRoot "runtime/$name"
    if (Test-Path -LiteralPath $binaryPath) { $binaryHashes[$name] = (Get-FileHash -LiteralPath $binaryPath -Algorithm SHA256).Hash }
}
$manifestPath = Join-Path $checkpointRoot 'manifest.json'
@{
    engine_base = $revision
    source_pins = Get-Content -LiteralPath (Join-Path $projectRoot 'upstream-lock.json') -Raw | ConvertFrom-Json
    binary_sha256 = $binaryHashes
    restore = 'Prepare a fresh project using the pinned sources and copied assets. Restore root source files and engine/src/khz from this archive; apply engine-working.patch to the clean engine base with git apply. Do not apply it over an already modified checkout. Assets, dependencies, binaries and Git history are excluded.'
} | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $manifestPath -Encoding utf8
$sources = @('CMakeLists.txt','README.md','.gitignore','upstream-lock.json','engine/LICENSE') |
    ForEach-Object { Join-Path $projectRoot $_ }
$sources += Get-ChildItem -LiteralPath $projectRoot -File -Filter '*.ps1' | ForEach-Object FullName
foreach ($directory in 'native','tests','config','scripts','docs','engine/src/khz') {
    $sources += Get-ChildItem -LiteralPath (Join-Path $projectRoot $directory) -File -Recurse |
        Where-Object { $_.Extension -in '.cpp','.hpp','.h','.ps1','.py','.cfg','.md' } |
        ForEach-Object FullName
}
Add-Type -AssemblyName System.IO.Compression.FileSystem
$archive = [System.IO.Compression.ZipFile]::Open($archivePath, [System.IO.Compression.ZipArchiveMode]::Create)
try {
    foreach ($path in ($sources | Sort-Object -Unique)) {
        $resolvedPath = (Resolve-Path -LiteralPath $path).Path
        if (!$resolvedPath.StartsWith($projectRoot + [System.IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
            throw "Source is outside the project: $resolvedPath"
        }
        $entry = $resolvedPath.Substring($projectRoot.Length + 1).Replace('\','/')
        [System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive, $resolvedPath, $entry) | Out-Null
    }
    [System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive, $patchPath, 'engine-working.patch') | Out-Null
    [System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive, $manifestPath, 'manifest.json') | Out-Null
}
finally { $archive.Dispose() }
$verification = [System.IO.Compression.ZipFile]::OpenRead($archivePath)
try {
    if (!$verification.GetEntry('engine-working.patch') -or !$verification.GetEntry('engine/src/khz/khz_runtime.cpp')) {
        throw 'Source checkpoint verification failed'
    }
    Write-Output "Source checkpoint: $archivePath ($($verification.Entries.Count) files)"
}
finally { $verification.Dispose() }
Get-FileHash -LiteralPath $archivePath -Algorithm SHA256 | Select-Object Hash,Path
