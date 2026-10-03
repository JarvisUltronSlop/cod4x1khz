# RenderFps 0 measures the maximum presentation rate independently of 1 kHz gameplay.
param([ValidateSet('mp_killhouse','mp_crash')][string]$Map = 'mp_killhouse', [ValidateRange(10,3600)][int]$Seconds = 60, [ValidateRange(0,3)][int]$HeadlessClients = 3, [ValidateRange(0,10000)][int]$RenderFps = 0, [ValidateSet('480p','720p')][string]$Resolution = '480p')
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'scripts/benchmark_provenance.ps1')
$renderLabel = if ($RenderFps -eq 0) { 'uncapped' } else { "$($RenderFps)fps" }
$runName = '{0}-{1}s-{2}-{3}-{4}' -f $Map,$Seconds,$renderLabel,$Resolution,(Get-Date -Format 'yyyyMMdd-HHmmss')
$outputFile = Join-Path $PSScriptRoot "results/$runName.json"
$provenanceFile = Join-Path $PSScriptRoot "results/$runName-provenance.json"
$provenance = [ordered]@{ schema = 1; passed = $false; before_launch = $null; after_cleanup = $null; owned_processes = @(); cleanup = @(); errors = @(); scope = 'Provenance hashes run outside the measured interval, before launch and after owned-process cleanup.' }
$ownedProcesses = @()
$ports = @(28962)
$failure = $null
try {
    $provenance.before_launch = Get-KHZBenchmarkSnapshot $PSScriptRoot
    if (!$provenance.before_launch.archive_matches) { throw ('Benchmark runtime does not match the latest archived build: ' + ($provenance.before_launch.archive_mismatches -join ', ')) }
    $provenance | ConvertTo-Json -Depth 20 | Set-Content -LiteralPath $provenanceFile -Encoding UTF8
    $server = & (Join-Path $PSScriptRoot 'launch-server.ps1') -Map $Map -Benchmark
    $ownedProcesses += $server
    $provenance.owned_processes += Get-KHZOwnedProcessEvidence $server (Join-Path $PSScriptRoot 'runtime/cod4x1khz_server.exe') 'dedicated_server'
    $client = & (Join-Path $PSScriptRoot 'launch-client.ps1') -Resolution $Resolution -RenderFps $RenderFps -Benchmark
    $ownedProcesses += $client
    $provenance.owned_processes += Get-KHZOwnedProcessEvidence $client (Join-Path $PSScriptRoot 'runtime/cod4x1khz.exe') 'rendered_client'
    for ($clientIndex = 1; $clientIndex -le $HeadlessClients; $clientIndex++) {
        $port = 28962 + $clientIndex
        $ports += $port
        $headless = & (Join-Path $PSScriptRoot 'launch-bench.ps1') -Port $port -Name "khz-bench-$clientIndex"
        $ownedProcesses += $headless
        $provenance.owned_processes += Get-KHZOwnedProcessEvidence $headless (Join-Path $PSScriptRoot 'runtime/cod4x1khz_bench.exe') "headless_client_$clientIndex"
    }
    & python (Join-Path $PSScriptRoot 'scripts/wait_ready.py') --ports @ports --server-port 28960 --desired-bots 15
    if ($LASTEXITCODE) { throw 'Clients failed to reach active gameplay' }
    Start-Sleep -Seconds 10
    & python (Join-Path $PSScriptRoot 'scripts/reset_server_stats.py')
    if ($LASTEXITCODE) { throw 'Could not reset the server work histogram after warmup' }
    $processIds = @($ownedProcesses | ForEach-Object { $_.Id })
    & python (Join-Path $PSScriptRoot 'scripts/monitor.py') --seconds $Seconds --client-ports @ports --pids @processIds --output $outputFile --map $Map --render-fps $RenderFps --resolution $Resolution
    if ($LASTEXITCODE) { throw "Benchmark incomplete; see $outputFile" }
    $report = Get-Content -LiteralPath $outputFile -Raw | ConvertFrom-Json
    if (!$report.summary.rates_pass -or !$report.summary.p99_pass -or !$report.summary.debt_pass -or !$report.summary.gameplay_pass -or !$report.summary.independent_owner_pass -or !$report.summary.bots_pass -or !$report.summary.collision_pass -or !$report.summary.script_pass -or !$report.summary.render_pass) { throw "Rate, work, backlog, gameplay, independent owner, bots, collision, script, or GPU present check failed; see $outputFile" }
}
catch { $failure = $_ }
finally {
    foreach ($process in $ownedProcesses) {
        try {
            if (!$process.HasExited) { $process.Kill(); [void]$process.WaitForExit(5000) }
            if (!$process.HasExited) { throw "Exact owned process $($process.Id) did not exit" }
            $provenance.cleanup += @{ pid = $process.Id; exited = $true; exit_code = $process.ExitCode }
        }
        catch { $provenance.errors += $_.Exception.Message; $provenance.cleanup += @{ pid = $process.Id; exited = $false }; if (!$failure) { $failure = $_ } }
        finally { $process.Dispose() }
    }
    if ($provenance.before_launch) {
        try {
            $archivePath = Join-Path $PSScriptRoot $provenance.before_launch.source_archive.path
            $provenance.after_cleanup = Get-KHZBenchmarkSnapshot $PSScriptRoot $archivePath
            $provenance.comparison = Compare-KHZBenchmarkSnapshots $provenance.before_launch $provenance.after_cleanup
            $provenance.passed = $provenance.before_launch.archive_matches -and $provenance.after_cleanup.archive_matches -and $provenance.comparison.passed -and $provenance.owned_processes.Count -eq (2 + $HeadlessClients) -and @($provenance.owned_processes | Where-Object { !$_.image_verified }).Count -eq 0 -and @($provenance.cleanup | Where-Object { !$_.exited }).Count -eq 0 -and $provenance.errors.Count -eq 0
            if (!$provenance.comparison.passed) { throw ('Benchmark files changed while owned game processes were alive: ' + ($provenance.comparison.changed_files -join ', ')) }
        }
        catch { $provenance.errors += $_.Exception.Message; if (!$failure) { $failure = $_ } }
    }
    $provenance | ConvertTo-Json -Depth 20 | Set-Content -LiteralPath $provenanceFile -Encoding UTF8
    # Preserve monitor samples, errors, and acceptance summaries on every path.
    if (Test-Path -LiteralPath $outputFile) { $report = Get-Content -LiteralPath $outputFile -Raw | ConvertFrom-Json -AsHashtable -Depth 100 }
    else { $report = @{ samples = @(); errors = @(); summary = @{}; benchmark_incomplete = $true } }
    if (!$report.Contains('summary') -or $null -eq $report['summary']) { $report['summary'] = @{} }
    $report['provenance'] = $provenance
    $report['provenance_file'] = $provenanceFile
    $report['render_workload'] = @{ camera = 'Legacy automated cardinal rotation, including occluded views and death/killcam periods'; representative_gameplay_fps = $false; camera_verified_script = 'test-render-scenes.ps1'; scope = 'This fixture stresses gameplay timing. Its mixed-camera FPS average is not representative normal-play throughput.' }
    $report['summary']['provenance_pass'] = $provenance.passed
    $wrapperErrors = @($provenance.errors)
    if ($failure) { $wrapperErrors += $failure.Exception.Message }
    $report['benchmark_wrapper_errors'] = $wrapperErrors
    $report | ConvertTo-Json -Depth 100 | Set-Content -LiteralPath $outputFile -Encoding UTF8
}
if ($failure) { throw $failure }
if (!$provenance.passed) { throw "Benchmark provenance failed; see $provenanceFile" }
Write-Output "Rate, work, backlog, gameplay, independent owner, bots, collision, script, GPU present and provenance checks passed: $outputFile"
