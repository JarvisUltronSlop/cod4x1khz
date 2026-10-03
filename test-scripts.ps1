param([ValidateSet('mp_killhouse','mp_crash')][string]$Map = 'mp_killhouse')
$ErrorActionPreference = 'Stop'
$server = & (Join-Path $PSScriptRoot 'launch-server.ps1') -Map $Map -Port 28961 -ScriptTests
try {
    & python (Join-Path $PSScriptRoot 'scripts/script_probe.py') --port 28961 --output (Join-Path $PSScriptRoot "results/script-probe-$Map.json")
    if ($LASTEXITCODE) { throw 'In-game script scheduling probe failed' }
}
finally {
    if (!$server.HasExited) { Stop-Process -Id $server.Id -ErrorAction SilentlyContinue }
}
