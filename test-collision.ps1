param([ValidateCount(1,2)][ValidateSet('mp_killhouse','mp_crash')][string[]]$Map = @('mp_killhouse','mp_crash'), [ValidateRange(0,3)][int]$HeadlessClients = 0)
$ErrorActionPreference = 'Stop'
foreach ($selectedMap in $Map) {
    # Refuse to join a pre-existing process accidentally. Release these temporary
    # sockets before the owned server/client bind their diagnostic ports.
    foreach ($port in (28981..(28982 + $HeadlessClients))) {
        $portProbe = [System.Net.Sockets.UdpClient]::new()
        try {
            $portProbe.Client.ExclusiveAddressUse = $true
            $portProbe.Client.Bind([System.Net.IPEndPoint]::new([System.Net.IPAddress]::Loopback, $port))
        }
        catch { throw "Collision diagnostic UDP port $port is occupied: $($_.Exception.Message)" }
        finally { $portProbe.Dispose() }
    }
    $ownedProcesses = @()
    $runName = 'collision-shadow-{0}-{1}' -f $selectedMap,(Get-Date -Format 'yyyyMMdd-HHmmss')
    $outputFile = Join-Path $PSScriptRoot "results/$runName.json"
    try {
        $ownedProcesses += & (Join-Path $PSScriptRoot 'launch-server.ps1') -Map $selectedMap -Port 28981 -TestControls
        $ownedProcesses += & (Join-Path $PSScriptRoot 'launch-client.ps1') -Server '127.0.0.1:28981' -Port 28982 -Resolution 720p -RenderFps 0 -Benchmark -TestControls
        $clientPorts = @(28982)
        for ($index = 1; $index -le $HeadlessClients; ++$index) {
            $clientPort = 28982 + $index
            $clientPorts += $clientPort
            $ownedProcesses += & (Join-Path $PSScriptRoot 'launch-bench.ps1') -Server '127.0.0.1:28981' -Port $clientPort -Name "collision-$index"
        }
        & python (Join-Path $PSScriptRoot 'scripts/wait_ready.py') --ports @clientPorts --timeout 30
        if ($LASTEXITCODE) { throw "Collision shadow check requires a spawned player on $selectedMap" }
        Start-Sleep -Seconds 2
        & python (Join-Path $PSScriptRoot 'scripts/collision_probe.py') --client-port 28982 --map $selectedMap --headless-clients $HeadlessClients --output $outputFile
        if ($LASTEXITCODE) { throw "Collision shadow diagnostic failed; see $outputFile" }
        Write-Output "Collision shadow diagnostic passed: $outputFile"
    }
    finally {
        foreach ($process in $ownedProcesses) {
            if (!$process.HasExited) { Stop-Process -Id $process.Id -ErrorAction SilentlyContinue }
            if (!$process.WaitForExit(10000)) { throw "Diagnostic process $($process.Id) did not exit" }
        }
    }
}
