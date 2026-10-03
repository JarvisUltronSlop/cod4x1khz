param([ValidateSet('mp_killhouse','mp_crash')][string]$Map = 'mp_killhouse')
$ErrorActionPreference = 'Stop'
$ownedProcesses = @()
$outputFile = Join-Path $PSScriptRoot "results/resilience-$Map.json"
try {
    $ownedProcesses += & (Join-Path $PSScriptRoot 'launch-server.ps1') -Map $Map -Port 28961 -TestControls
    $client = & (Join-Path $PSScriptRoot 'launch-bench.ps1') -Server '127.0.0.1:28961' -Port 28974 -Name 'khz-resilience'
    $ownedProcesses += $client
    & python (Join-Path $PSScriptRoot 'scripts/wait_ready.py') --ports 28974 --timeout 30
    if ($LASTEXITCODE) { throw 'Resilience checks require a spawned player' }
    & python (Join-Path $PSScriptRoot 'scripts/resilience_probe.py') --phase transition --map $Map --output $outputFile
    if ($LASTEXITCODE) { throw "Transition checks failed; see $outputFile" }
    if (!$client.HasExited) { Stop-Process -Id $client.Id }
    $ownedProcesses += & (Join-Path $PSScriptRoot 'launch-bench.ps1') -Server '127.0.0.1:28961' -Port 28974 -Name 'khz-reconnect'
    & python (Join-Path $PSScriptRoot 'scripts/wait_ready.py') --ports 28974 --timeout 30
    if ($LASTEXITCODE) { throw 'Replacement client did not reconnect and spawn' }
    & python (Join-Path $PSScriptRoot 'scripts/resilience_probe.py') --phase reconnect --output $outputFile
    if ($LASTEXITCODE) { throw "Reconnect checks failed; see $outputFile" }
}
finally {
    foreach ($process in $ownedProcesses) {
        if (!$process.HasExited) { Stop-Process -Id $process.Id -ErrorAction SilentlyContinue }
    }
}
