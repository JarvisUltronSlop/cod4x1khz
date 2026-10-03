param([Parameter(Mandatory)][string]$ConsoleJsonl)
$ErrorActionPreference = 'Stop'
$taskRoot = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$taskWriter = [IO.StreamWriter]::new($ConsoleJsonl, $false, [Text.UTF8Encoding]::new($false))
try {
    & (Join-Path $taskRoot 'benchmark.ps1') -Map mp_killhouse -Seconds 60 -HeadlessClients 0 -RenderFps 0 -Resolution 480p |
        ForEach-Object {
            $taskLine = $_.ToString()
            $taskWriter.WriteLine((@{ recorded_utc = [DateTime]::UtcNow.ToString('o'); line = $taskLine } | ConvertTo-Json -Compress))
            $taskWriter.Flush()
            if ($taskLine -match '^\d+\.\d+s:|checks passed:') { Write-Output $taskLine }
        }
}
finally { $taskWriter.Dispose() }
