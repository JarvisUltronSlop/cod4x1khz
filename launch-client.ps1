# RenderFps 0 leaves presentation uncapped. Gameplay remains fixed at 1,000 Hz.
# Change the presentation cap while playing with: com_maxfps <fps> (0 = uncapped).
param([string]$Server = '127.0.0.1:28960', [ValidateSet('1080p','720p','480p')][string]$Resolution = '720p', [ValidateRange(0,10000)][int]$RenderFps = 0, [ValidateRange(1024,65535)][int]$Port = 28962, [switch]$Benchmark, [switch]$TestControls, [string]$Configuration = 'Release', [ValidateSet('Win32','x64')][string]$Architecture = 'Win32', [ValidateSet('d3d9','vulkan')][string]$Renderer = 'd3d9', [switch]$Throughput)
$ErrorActionPreference = 'Stop'
if ($Throughput) {
    if ($PSBoundParameters.ContainsKey('Architecture') -and $Architecture -ne 'x64') { throw 'Throughput requires the native x64 client' }
    if ($PSBoundParameters.ContainsKey('Renderer') -and $Renderer -ne 'vulkan') { throw 'Throughput requires Vulkan' }
    $Architecture = 'x64'
    $Renderer = 'vulkan'
}
$runtimeRoot = Join-Path $PSScriptRoot 'runtime'
$executableRoot = if ($Architecture -eq 'x64') { Join-Path $runtimeRoot 'x64' } else { $runtimeRoot }
$executable = Join-Path $executableRoot 'cod4x1khz.exe'
if (!(Test-Path -LiteralPath $executable)) { throw 'Build the client with build.ps1 first' }
$dimensions = switch ($Resolution) { '1080p' { '1920x1080' } '720p' { '1280x720' } '480p' { '640x480' } }
$arguments = @('+set','net_port',"$Port")
if ($Benchmark) {
    $arguments += @('+set','khz_benchmark','1','+set','com_introPlayed','1','+set','r_customMode',$dimensions,'+set','r_mode',$dimensions,'+set','r_fullscreen','0','+exec','client.cfg','+set','com_maxfps',"$RenderFps")
} else {
    if ($Throughput -or $PSBoundParameters.ContainsKey('RenderFps')) { $arguments += @('+set','com_maxfps',"$RenderFps") }
    if ($PSBoundParameters.ContainsKey('Resolution')) { $arguments += @('+set','r_customMode',$dimensions,'+set','r_mode',$dimensions) }
}
if ($TestControls) { $arguments += @('+set','khz_test_controls','1') }
if ($Throughput -or $PSBoundParameters.ContainsKey('Renderer')) { $arguments += @('+set','r_graphicsApi',$Renderer) }
if ($Throughput) {
    $arguments += @('+set','r_vsync','0','+set','r_vkFramesInFlight','3',
        '+set','r_vkLegacySwapFence','0','+set','r_vkLegacySceneFence','0',
        '+set','r_vkOverlapUploads','1','+set','r_vkAsyncSubmit','1',
        '+set','r_overlapFrontend','1','+set','r_asyncSunQueries','1',
        '+set','snd_asyncStreams','1','+set','winvoice_capturePollMs','5',
        '+set','r_altModelLightingUpdate','1')
}
if ($Benchmark -or $PSBoundParameters.ContainsKey('Server')) { $arguments += @('+connect',$Server) }
Start-Process -FilePath $executable -ArgumentList $arguments -WorkingDirectory $runtimeRoot -PassThru
