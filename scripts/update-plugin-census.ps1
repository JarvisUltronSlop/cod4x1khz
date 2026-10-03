[CmdletBinding()]
param([string]$ProjectRoot = (Split-Path $PSScriptRoot -Parent))
$ErrorActionPreference = 'Stop'
$ProjectRoot = [IO.Path]::GetFullPath($ProjectRoot)
$outputPath = Join-Path $ProjectRoot 'docs/public-plugin-coverage.json'
$manifest = Get-Content -LiteralPath $outputPath -Raw | ConvertFrom-Json -AsHashtable
$entries = [Collections.Generic.Dictionary[string, object]]::new([StringComparer]::Ordinal)
foreach ($entry in $manifest.exports) { $entries[$entry.name] = $entry }
$sources = [Collections.Generic.Dictionary[string, object]]::new([StringComparer]::Ordinal)
function Add-Export([string]$Name, [string]$File, [int]$Line, [string]$Kind) {
    if (-not $sources.ContainsKey($Name)) { $sources[$Name] = [Collections.Generic.List[object]]::new() }
    $sources[$Name].Add([ordered]@{ source = "${File}:$Line"; kind = $Kind })
}
function Without-CComments([string]$Text) {
    # Preserve line numbers when removing examples and deprecated declarations.
    [regex]::Replace($Text, '(?s)/\*.*?\*/|(?m)//[^\r\n]*', {
        param($match) [regex]::Replace($match.Value, '[^\r\n]', ' ')
    })
}
$declarations = 'upstream/cod4x_server/plugins/function_declarations.h'
$text = Without-CComments (Get-Content -LiteralPath (Join-Path $ProjectRoot $declarations) -Raw)
foreach ($match in [regex]::Matches($text, '\b(Plugin_[A-Za-z0-9_]+)\s*\(')) {
    Add-Export $match.Groups[1].Value $declarations (1 + ([regex]::Matches($text.Substring(0, $match.Index), "`n")).Count) 'declaration'
}
$assembly = 'upstream/cod4x_server/src/asmsource/pluginexports.asm'
$lines = Get-Content -LiteralPath (Join-Path $ProjectRoot $assembly)
for ($i = 0; $i -lt $lines.Count; ++$i) {
    $line = ($lines[$i] -split ';', 2)[0].Trim()
    if ($line -match '^pexport\s+([A-Za-z_][A-Za-z0-9_]*)\s*$') {
        Add-Export ('Plugin_' + $Matches[1]) $assembly ($i + 1) 'pexport'
    } elseif ($line -match '^ralias\s+([A-Za-z_][A-Za-z0-9_]*)\s*,') {
        Add-Export $Matches[1] $assembly ($i + 1) 'ralias'
    }
}
foreach ($path in Get-ChildItem -LiteralPath (Join-Path $ProjectRoot 'upstream/cod4x_server/src') -Recurse -File | Where-Object Extension -in '.c', '.cpp') {
    $relative = [IO.Path]::GetRelativePath($ProjectRoot, $path.FullName).Replace('\', '/')
    $text = Without-CComments (Get-Content -LiteralPath $path.FullName -Raw)
    foreach ($match in [regex]::Matches($text, '(?m)^[ \t]*P_P_F[ \t]+[^\r\n;{}]*?\b([A-Za-z_][A-Za-z0-9_]*)\s*\(')) {
        Add-Export $match.Groups[1].Value $relative (1 + ([regex]::Matches($text.Substring(0, $match.Index), "`n")).Count) 'P_P_F'
    }
}
foreach ($required in 'Plugin_FS_FreeFile', 'Plugin_Scr_AllocArray', 'Plugin_unzSetPassword', 'Plugin_IsLoaded', 'Plugin_GetVersion', 'Plugin_NET_AdrToString', 'Plugin_AddCommandForClientToWhitelist') {
    if (-not $sources.ContainsKey($required)) { throw "Plugin census missed active export $required" }
}
if ($sources.ContainsKey('Plugin_Cvar_RegisterEnum')) { throw 'Plugin census included a commented declaration/assembly invocation' }
$exports = foreach ($name in ($sources.Keys | Sort-Object -CaseSensitive)) {
    if ($entries.ContainsKey($name)) { $entry = $entries[$name] }
    else { $entry = [ordered]@{ name = $name; status = 'pending'; pinned_source = $sources[$name][0].source; implementation = $null; semantic_evidence = @() } }
    $entry.pinned_sources = @($sources[$name])
    $entry.pinned_source = $sources[$name][0].source
    if ($name -eq 'Plugin_AddCommandForClientToWhitelist') { $entry.status = 'implemented_unverified'; $entry.implementation = 'engine/src/khz/khz_plugins.cpp' }
    $entry
}
$manifest.exports = @($exports)
$manifest.census = [ordered]@{
    method = 'Union of active declarations, pexport/ralias invocations, and P_P_F definitions; comments excluded. Source presence is not semantic parity.'
    generated_by = 'scripts/update-plugin-census.ps1'
    union_count = $exports.Count
    declaration_count = @($sources.Keys | Where-Object { @($sources[$_] | Where-Object kind -eq 'declaration').Count }).Count
    assembly_count = @($sources.Keys | Where-Object { @($sources[$_] | Where-Object kind -in 'pexport', 'ralias').Count }).Count
    definition_count = @($sources.Keys | Where-Object { @($sources[$_] | Where-Object kind -eq 'P_P_F').Count }).Count
}
$manifest.fixtures = @($manifest.fixtures + 'tests/identity_admin_tests.cpp' + 'sdk/tests/header_smoke.c' | Select-Object -Unique)
$json = $manifest | ConvertTo-Json -Depth 12
[IO.File]::WriteAllText($outputPath, $json + "`n", [Text.UTF8Encoding]::new($false))
$manifest.census | ConvertTo-Json -Compress
