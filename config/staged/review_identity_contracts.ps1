param()
$ErrorActionPreference = 'Stop'
$projectPath = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '../..')).Path
$reviewPath = Join-Path $projectPath ('results/identity-contract-review-' + (Get-Date -Format 'yyyyMMdd-HHmmss-fffffff'))
New-Item -ItemType Directory -Path $reviewPath | Out-Null

& python (Join-Path $PSScriptRoot 'generate_identity_guid_goldens.py') 2>&1 |
    Tee-Object -FilePath (Join-Path $reviewPath 'python-goldens.log')
if ($LASTEXITCODE) { throw 'Independent Python golden generation failed' }

# A separate .NET implementation recomputes every vector from the pinned salt;
# it does not use the adapted CNG identity source or Python derived bytes.
$pinnedPath = Join-Path $projectPath 'upstream/cod4x_server/src/sapi.c'
$pinnedText = Get-Content -LiteralPath $pinnedPath -Raw
$saltBody = [regex]::Match($pinnedText, 'static\s+const\s+unsigned\s+char\s+playerid_salt\[\]\s*=\s*\{([^}]+)\}')
if (!$saltBody.Success) { throw 'Pinned salt declaration missing in .NET review' }
[byte[]]$saltBytes = @([regex]::Matches($saltBody.Groups[1].Value, '0x([0-9a-fA-F]{2})') |
    ForEach-Object { [Convert]::ToByte($_.Groups[1].Value, 16) })
if ($saltBytes.Length -ne 256) { throw 'Pinned salt must have 256 bytes' }
$oracle = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'identity_guid_goldens.json') -Raw | ConvertFrom-Json
$dotNetRecords = @()
foreach ($record in $oracle.records) {
    [byte[]]$decoded = [Convert]::FromHexString($record.guid)
    $crypto = [System.Security.Cryptography.Rfc2898DeriveBytes]::new($decoded, $saltBytes, 100,
        [System.Security.Cryptography.HashAlgorithmName]::SHA256)
    try { [byte[]]$derived = $crypto.GetBytes(4) } finally { $crypto.Dispose() }
    $derivedHex = [Convert]::ToHexString($derived).ToLowerInvariant()
    [uint64]$actual = [uint64]2310346613136031744 + [uint64][BitConverter]::ToUInt32($derived, 0)
    if ($actual -ne [uint64]$record.expected -or $derivedHex -cne $record.derived_hex) {
        throw ('Independent .NET golden disagreement: ' + $record.guid)
    }
    $dotNetRecords += [ordered]@{ guid=$record.guid; derived_hex=$derivedHex; actual=$actual; passed=$true }
}
[ordered]@{ passed=$true; vectors=$dotNetRecords.Count; records=$dotNetRecords; native_sdk_execution=$false } |
    ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $reviewPath 'dotnet-goldens.json') -Encoding utf8

& python (Join-Path $PSScriptRoot 'verify_identity_contract_drafts.py') 2>&1 |
    Tee-Object -FilePath (Join-Path $reviewPath 'oracle-contracts.log')
if ($LASTEXITCODE) { throw 'Focused independent oracle contract checks failed' }
& python (Join-Path $projectPath 'tests/plugin_fixture_registration_tests.py') 2>&1 |
    Tee-Object -FilePath (Join-Path $reviewPath 'fixture-registration.log')
if ($LASTEXITCODE) { throw 'Pinned twenty-command fixture inventory checks failed' }

$installationPath = & 'C:/Program Files (x86)/Microsoft Visual Studio/Installer/vswhere.exe' -latest -products '*' -property installationPath
if (!$installationPath) { throw 'Installed MSVC toolchain missing' }
$vcvarsPath = Join-Path $installationPath.Trim() 'VC/Auxiliary/Build/vcvarsall.bat'
if (!(Test-Path -LiteralPath $vcvarsPath)) { throw 'MSVC x86 environment script missing' }
$syntaxScript = Join-Path $reviewPath 'fixture-syntax.cmd'
@"
@echo off
call "$vcvarsPath" x86 >nul
if errorlevel 1 exit /b 1
cl.exe /nologo /Zs /TC /I "$projectPath/sdk/include" "$projectPath/sdk/tests/regression_plugin.c"
"@ | Set-Content -LiteralPath $syntaxScript -Encoding ascii
& $env:ComSpec /d /c $syntaxScript 2>&1 |
    Tee-Object -FilePath (Join-Path $reviewPath 'fixture-syntax.log')
if ($LASTEXITCODE) { throw 'MSVC x86 fixture C syntax failed' }

$sourceNames = @('sdk/tests/regression_plugin.c', 'sdk/tests/native_identity_contract_probe.inc',
    'sdk/tests/identity_guid_goldens.inc', 'sdk/tests/native_identity_format_probe.inc',
    'engine/src/khz/khz_identity.cpp', 'engine/src/khz/khz_plugins.cpp',
    'native/plugin_registry.hpp', 'upstream/cod4x_server/src/sapi.c',
    'upstream/cod4x_server/src/plugin_handler.h',
    'config/staged/generate_identity_guid_goldens.py', 'config/staged/identity_guid_goldens.json',
    'config/staged/identity_guid_goldens.inc', 'config/staged/verify_identity_contract_drafts.py',
    'config/staged/review_identity_contracts.ps1')
$sourceHashes = [ordered]@{}
foreach ($name in $sourceNames) {
    $sourceHashes[$name] = (Get-FileHash -LiteralPath (Join-Path $projectPath $name) -Algorithm SHA256).Hash.ToLowerInvariant()
}
$logHashes = [ordered]@{}
foreach ($name in @('python-goldens.log', 'dotnet-goldens.json', 'oracle-contracts.log',
                   'fixture-registration.log', 'fixture-syntax.log', 'fixture-syntax.cmd')) {
    $logHashes[$name] = (Get-FileHash -LiteralPath (Join-Path $reviewPath $name) -Algorithm SHA256).Hash.ToLowerInvariant()
}
$result = [ordered]@{
    schema=1; passed=$true; preparation_only=$true; semantic_proof=$false;
    native_sdk_execution=$false; engine_build=$false; game_processes_started=0;
    classifier_expected_cases_per_export=32838; guid_vectors=7; guid_expected_native_cases=247;
    golden_oracles=@('Python hashlib PBKDF2-HMAC-SHA256', '.NET Rfc2898DeriveBytes SHA256');
    fixture_c_syntax_passed=$true; fixture_registration_tests_passed=$true;
    official_provider_authentication=$false; source_sha256=$sourceHashes; log_sha256=$logHashes
}
$result | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $reviewPath 'result.json') -Encoding utf8
Write-Output ('Identity offline review PASS: ' + (Join-Path $reviewPath 'result.json'))
