param(
    [Parameter(Mandatory)][int]$Build,
    [Parameter(Mandatory)][int]$NativeSuites,
    [Parameter(Mandatory)][string]$Scope
)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$logRelative = "results/build$Build-console.log"
$logPath = Join-Path $projectRoot $logRelative
$logText = Get-Content -LiteralPath $logPath -Raw
if ($logText -notmatch "100% tests passed(?:, 0 tests failed)? out of $NativeSuites\b") {
    throw "Build $Build lacks the expected successful native test summary"
}
$binaries = [ordered]@{}
foreach ($relative in @('cod4x1khz.exe', 'runtime/cod4x1khz.exe',
        'runtime/cod4x1khz_server.exe', 'runtime/cod4x1khz_bench.exe',
        'runtime/cod4x1khz_update.exe', 'runtime/plugins/khz_regression.dll',
        'mss32.dll', 'D3DX9_43.dll', 'binkw32.dll', 'steam_api.dll',
        'runtime/mss32.dll', 'runtime/D3DX9_43.dll', 'runtime/binkw32.dll', 'runtime/steam_api.dll')) {
    $binaries[$relative] = (Get-FileHash -LiteralPath (Join-Path $projectRoot $relative) -Algorithm SHA256).Hash.ToLowerInvariant()
}
$sourceFiles = [System.Collections.Generic.SortedSet[string]]::new([System.StringComparer]::Ordinal)
foreach ($directory in @('engine/src', 'engine/tests', 'native', 'tests', 'sdk', 'config', 'scripts')) {
    foreach ($file in Get-ChildItem -LiteralPath (Join-Path $projectRoot $directory) -File -Recurse) {
        if ($file.Extension -in @('.cpp', '.c', '.h', '.hpp', '.inl', '.inc', '.gsc', '.cfg', '.json', '.ps1', '.py', '.rc', '.ico') -and
            $file.FullName -notmatch '[\\/]__pycache__[\\/]') {
            [void]$sourceFiles.Add([IO.Path]::GetRelativePath($projectRoot, $file.FullName).Replace('\', '/'))
        }
    }
}
foreach ($relative in @('CMakeLists.txt', 'engine/CMakeLists.txt', 'build.ps1', 'upstream-lock.json', 'test-render-scenes.ps1')) {
    [void]$sourceFiles.Add($relative)
}
$sources = [ordered]@{}
foreach ($relative in $sourceFiles) {
    $sources[$relative] = (Get-FileHash -LiteralPath (Join-Path $projectRoot $relative) -Algorithm SHA256).Hash.ToLowerInvariant()
}
$evidence = [ordered]@{
    build = $Build
    recorded_utc = [DateTime]::UtcNow.ToString('o')
    source_pins = (Get-Content -LiteralPath (Join-Path $projectRoot 'upstream-lock.json') -Raw | ConvertFrom-Json)
    release_gate_passed = $false
    scope = $Scope
    tests = @{ native_suites = $NativeSuites; passed = $true; log = $logRelative }
    binary_sha256 = $binaries
    runtime_asset_packages = @{
        'runtime/main/zz_cod4x1khz_bots.iwd' = (Get-FileHash -LiteralPath (Join-Path $projectRoot 'runtime/main/zz_cod4x1khz_bots.iwd') -Algorithm SHA256).Hash.ToLowerInvariant()
    }
    source_inventory_note = 'Hashes record project source at archival time; staged files absent from CMake/native includes are not claimed as compiled or tested.'
    source_sha256 = $sources
}
$destination = Join-Path $projectRoot "results/build$Build-source-and-binary-hashes.json"
$evidence | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath $destination -Encoding UTF8
[pscustomobject]@{ Path = $destination; NativeSuites = $NativeSuites; BinarySHA256 = $binaries; SourceFiles = $sources.Count }
