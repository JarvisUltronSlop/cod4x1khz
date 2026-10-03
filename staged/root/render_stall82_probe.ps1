$ErrorActionPreference = 'Stop'
$projectRoot = 'C:\Users\jack\Desktop\cod4x1khz'
$owned = @()
try {
    $server = & (Join-Path $projectRoot 'launch-server.ps1') -Map mp_killhouse -Benchmark
    $owned += $server
    $client = & (Join-Path $projectRoot 'launch-client.ps1') -Resolution 480p -RenderFps 0 -Benchmark
    $owned += $client
    & python (Join-Path $projectRoot 'scripts/wait_ready.py') --ports 28962 --server-port 28960 --desired-bots 15
    if ($LASTEXITCODE) { throw 'Client readiness failed' }
    & python (Join-Path $PSScriptRoot 'render_stall82_probe.py') --client $client.Id --server $server.Id --output (Join-Path $projectRoot 'results/build82-render-stall-diagnosis')
    if ($LASTEXITCODE) { throw 'Stall inspector failed' }
}
finally {
    foreach ($process in $owned) {
        try {
            if (!$process.HasExited) { $process.Kill(); [void]$process.WaitForExit(5000) }
            Write-Output "Exact owned process $($process.Id) exited=$($process.HasExited) code=$($process.ExitCode)"
        } finally { $process.Dispose() }
    }
}
