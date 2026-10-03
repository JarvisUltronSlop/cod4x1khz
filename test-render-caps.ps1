param([ValidateSet('mp_killhouse','mp_crash')][string]$Map = 'mp_crash', [ValidateSet('480p','720p')][string]$Resolution = '720p')
$ErrorActionPreference = 'Stop'
$ownedProcesses = @()
$runName = 'render-caps-{0}-{1}-{2}' -f $Map,$Resolution,(Get-Date -Format 'yyyyMMdd-HHmmss')
$outputFile = Join-Path $PSScriptRoot "results/$runName.json"
try {
    $ownedProcesses += & (Join-Path $PSScriptRoot 'launch-server.ps1') -Map $Map -Port 28961 -TestControls
    $ownedProcesses += & (Join-Path $PSScriptRoot 'launch-client.ps1') -Server '127.0.0.1:28961' -Port 28975 -Resolution $Resolution -RenderFps 0 -Benchmark -TestControls
    & python (Join-Path $PSScriptRoot 'scripts/wait_ready.py') --ports 28975 --timeout 30
    if ($LASTEXITCODE) { throw 'Render-cap checks require a spawned player' }
    & python (Join-Path $PSScriptRoot 'scripts/render_cap_probe.py') --server-port 28961 --client-port 28975 --map $Map --resolution $Resolution --output $outputFile
    if ($LASTEXITCODE) { throw "Render-cap or fixed gameplay checks failed; see $outputFile" }
    Write-Output "Render-cap and fixed gameplay checks passed: $outputFile"
}
finally {
    foreach ($process in $ownedProcesses) {
        if (!$process.HasExited) { Stop-Process -Id $process.Id -ErrorAction SilentlyContinue }
    }
}
