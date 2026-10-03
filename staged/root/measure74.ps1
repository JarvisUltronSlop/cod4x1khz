$ErrorActionPreference = 'Stop'
$taskRoot = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$taskRecords = @()
$taskCases = @(
    @{ map='mp_killhouse'; headless=0 },
    @{ map='mp_crash'; headless=0 },
    @{ map='mp_killhouse'; headless=3 },
    @{ map='mp_crash'; headless=3 }
)
foreach ($taskCase in $taskCases) {
    $taskStart = [DateTime]::UtcNow
    $taskError = $null
    try { & (Join-Path $taskRoot 'benchmark.ps1') -Map $taskCase.map -Seconds 60 -HeadlessClients $taskCase.headless -RenderFps 0 -Resolution 480p }
    catch { $taskError = $_.Exception.Message; Write-Output ('Case failed: ' + $taskError) }
    $taskPattern = '^' + $taskCase.map + '-60s-uncapped-480p-[0-9]{8}-[0-9]{6}\.json$'
    $taskOutputs = @(Get-ChildItem -LiteralPath (Join-Path $taskRoot 'results') -File |
        Where-Object { $_.Name -match $taskPattern -and $_.LastWriteTimeUtc -ge $taskStart })
    $taskRecords += @{ map=$taskCase.map; humans=(1 + $taskCase.headless); bots=(15 - $taskCase.headless);
        wrapper_pass=($null -eq $taskError); error=$taskError;
        reports=@($taskOutputs | ForEach-Object { 'results/' + $_.Name }); started_utc=$taskStart.ToString('o'); ended_utc=[DateTime]::UtcNow.ToString('o') }
}
$taskOutput = Join-Path $taskRoot 'results/build74-timing-cases.json'
@{ build=74; cases=$taskRecords; passed=(@($taskRecords | Where-Object { !$_.wrapper_pass -or $_.reports.Count -ne 1 }).Count -eq 0);
    scope='Four serial actual 60-second matches; native archive oracle and full-world replay remain separate gates.' } |
    ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $taskOutput -Encoding UTF8
Write-Output ('All four cases recorded: ' + $taskOutput)
