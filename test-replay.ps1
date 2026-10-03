param([ValidateSet('mp_killhouse','mp_crash')][string]$Map = 'mp_killhouse')
$ErrorActionPreference = 'Stop'
$ownedProcesses = @()
try {
    $ownedProcesses += & (Join-Path $PSScriptRoot 'launch-server.ps1') -Map $Map -Port 28961
    $ownedProcesses += & (Join-Path $PSScriptRoot 'launch-bench.ps1') -Server '127.0.0.1:28961' -Port 28973 -Name 'khz-replay'
    & python (Join-Path $PSScriptRoot 'scripts/wait_ready.py') --ports 28973 --timeout 30
    if ($LASTEXITCODE) { throw 'Replay requires a spawned player' }
    & python (Join-Path $PSScriptRoot 'scripts/replay_probe.py') --output (Join-Path $PSScriptRoot "results/movement-replay-$Map.json")
    if ($LASTEXITCODE) { throw 'Movement replay diverged across presentation cadences' }
}
finally {
    foreach ($process in $ownedProcesses) {
        if (!$process.HasExited) { Stop-Process -Id $process.Id -ErrorAction SilentlyContinue }
    }
}
