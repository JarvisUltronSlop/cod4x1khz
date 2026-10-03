param([ValidateSet('mp_killhouse','mp_crash')][string]$Map = 'mp_killhouse', [switch]$SkipNetwork)
$ErrorActionPreference = 'Stop'
$coreTests = Join-Path $PSScriptRoot 'build/Release/Release/khz_tests.exe'
if (!(Test-Path -LiteralPath $coreTests)) { throw 'Run build.ps1 before test-quick.ps1' }
& $coreTests
if ($LASTEXITCODE) { throw 'Core scheduling, queue, and protocol checks failed' }
$publicationTests = Join-Path $PSScriptRoot 'build/Release/Release/khz_publication_tests.exe'
if (!(Test-Path -LiteralPath $publicationTests)) { throw 'Run build.ps1 before test-quick.ps1' }
& $publicationTests
if ($LASTEXITCODE) { throw 'Immutable publication ownership checks failed' }
& python -m unittest discover -s (Join-Path $PSScriptRoot 'tests') -p test_telemetry_query.py
if ($LASTEXITCODE) { throw 'Telemetry fragmentation and bounded query checks failed' }
& (Join-Path $PSScriptRoot 'test-scripts.ps1') -Map $Map
& (Join-Path $PSScriptRoot 'test-replay.ps1') -Map $Map
& (Join-Path $PSScriptRoot 'test-resilience.ps1') -Map $Map
if (!$SkipNetwork) { & (Join-Path $PSScriptRoot 'test-network.ps1') -Map $Map }
Write-Output 'Quick regression checks passed. Use benchmark.ps1 for a separate 60-second match measurement.'
