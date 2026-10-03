param(
    [Parameter(Mandatory)][string]$Output,
    [ValidateRange(1,180)][int]$Samples = 55,
    [ValidateRange(1,5)][int]$IntervalSeconds = 2
)
$ErrorActionPreference = 'Stop'
# Read-only observer: CPU counters run outside the game processes. The benchmark
# already records process/thread CPU times and per-process GPU engine counters.
$taskRecords = [System.Collections.Generic.List[object]]::new()
try {
    Get-Counter -Counter '\Processor(*)\% Processor Time' -SampleInterval $IntervalSeconds -MaxSamples $Samples |
        ForEach-Object {
            $taskValues = [ordered]@{}
            foreach ($taskCounter in $_.CounterSamples) {
                $taskValues[$taskCounter.InstanceName] = if ($taskCounter.Status -eq 0) { $taskCounter.CookedValue } else { $null }
            }
            $taskRecords.Add(@{
                recorded_utc = $_.Timestamp.ToUniversalTime().ToString('o')
                processor_busy_percent = $taskValues
            })
        }
}
finally {
    @{
        schema = 1
        interval_seconds = $IntervalSeconds
        samples = @($taskRecords.ToArray())
        scope = 'System-wide logical processor busy time; align timestamps with the benchmark console measurement markers. CPU values include other applications.'
    } | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $Output -Encoding UTF8
}
