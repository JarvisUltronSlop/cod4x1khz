# RenderFps 0 leaves presentation uncapped. Gameplay remains fixed at 1,000 Hz.
# Change the presentation cap while playing with: com_maxfps <fps> (0 = uncapped).
param([string]$Server = '127.0.0.1:28960', [ValidateSet('720p','480p')][string]$Resolution = '720p', [ValidateRange(0,10000)][int]$RenderFps = 0, [ValidateRange(1024,65535)][int]$Port = 28962, [switch]$Benchmark, [switch]$TestControls, [string]$Configuration = 'Release')
$ErrorActionPreference = 'Stop'
$runtimeRoot = Join-Path $PSScriptRoot 'runtime'
$executable = Join-Path $runtimeRoot 'cod4x1khz.exe'
if (!(Test-Path -LiteralPath $executable)) { throw 'Build the client with build.ps1 first' }
$dimensions = if ($Resolution -eq '720p') { '1280x720' } else { '640x480' }
$arguments = @('+set','net_port',"$Port")
if ($Benchmark) {
    $arguments += @('+set','khz_benchmark','1','+set','com_introPlayed','1','+set','r_customMode',$dimensions,'+set','r_mode',$dimensions,'+set','r_fullscreen','0','+exec','client.cfg','+set','com_maxfps',"$RenderFps")
} else {
    if ($PSBoundParameters.ContainsKey('RenderFps')) { $arguments += @('+set','com_maxfps',"$RenderFps") }
    if ($PSBoundParameters.ContainsKey('Resolution')) { $arguments += @('+set','r_customMode',$dimensions,'+set','r_mode',$dimensions) }
}
if ($TestControls) { $arguments += @('+set','khz_test_controls','1') }
if ($Benchmark -or $PSBoundParameters.ContainsKey('Server')) { $arguments += @('+connect',$Server) }
Start-Process -FilePath $executable -ArgumentList $arguments -WorkingDirectory $runtimeRoot -PassThru
