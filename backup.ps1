param(
    [string]$ReleaseTag,
    [ValidateSet('main','vulkan-testing')][string]$Branch = 'vulkan-testing',
    [switch]$Full,
    [switch]$InitialRelease
)
$ErrorActionPreference = 'Stop'
$arguments = @((Join-Path $PSScriptRoot 'scripts/github_backup.py'), '--branch', $Branch)
if ($ReleaseTag) { $arguments += @('--release', $ReleaseTag) }
if ($Full) { $arguments += '--full' }
if ($InitialRelease) { $arguments += '--initial-release' }
& python @arguments
if ($LASTEXITCODE) { throw 'GitHub backup did not complete; inspect .backup/ and retry without changing the release tag.' }
