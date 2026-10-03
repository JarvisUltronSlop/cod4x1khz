param([ValidateSet('mp_killhouse','mp_crash')][string]$Map = 'mp_killhouse')
$ErrorActionPreference = 'Stop'
$runName = 'network-{0}-{1}' -f $Map,(Get-Date -Format 'yyyyMMdd-HHmmss')
$resultsRoot = Join-Path $PSScriptRoot 'results'
$ownedProcesses = @()
try {
    $ownedProcesses += & (Join-Path $PSScriptRoot 'launch-server.ps1') -Map $Map -Port 28971
    $proxyPath = Join-Path $PSScriptRoot 'scripts/fault_proxy.py'
    $proxyOutput = Join-Path $resultsRoot "$runName-proxy.json"
    $proxy = Start-Process -FilePath 'python' -ArgumentList @("`"$proxyPath`"",'--output',"`"$proxyOutput`"") -WorkingDirectory $PSScriptRoot -WindowStyle Hidden -RedirectStandardOutput (Join-Path $resultsRoot "$runName-proxy.log") -RedirectStandardError (Join-Path $resultsRoot "$runName-proxy-errors.log") -PassThru
    $ownedProcesses += $proxy
    $ownedProcesses += & (Join-Path $PSScriptRoot 'launch-bench.ps1') -Server '127.0.0.1:28970' -Port 28972 -Name 'khz-network'
    & python (Join-Path $PSScriptRoot 'scripts/wait_ready.py') --ports 28972 --timeout 7
    if ($LASTEXITCODE) { throw 'Client did not spawn before impairment began' }
    & python (Join-Path $PSScriptRoot 'scripts/network_probe.py') --proxy-output $proxyOutput --output (Join-Path $resultsRoot "$runName.json")
    if ($LASTEXITCODE) { throw 'Network recovery checks failed' }
}
finally {
    foreach ($process in $ownedProcesses) {
        if (!$process.HasExited) { Stop-Process -Id $process.Id -ErrorAction SilentlyContinue }
    }
}
