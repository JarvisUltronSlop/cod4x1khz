param([string]$Server = '127.0.0.1:28960', [ValidateRange(1024,65535)][int]$Port = 28963, [string]$Name = 'khz-bench')
$ErrorActionPreference = 'Stop'
if ($Name -notmatch '^[A-Za-z0-9_-]{1,24}$') { throw 'Use a short benchmark name with letters, digits, underscores, or hyphens' }
$runtimeRoot = Join-Path $PSScriptRoot 'runtime'
$executable = Join-Path $runtimeRoot 'cod4x1khz_bench.exe'
if (!(Test-Path -LiteralPath $executable)) { throw 'Run build.ps1 first' }
$arguments = @('+set','net_port',"$Port",'+set','com_introPlayed','1','+exec','client.cfg','+set','r_customMode','640x480','+set','r_mode','640x480','+set','r_fullscreen','0','+set','name',$Name,'+connect',$Server)
Start-Process -FilePath $executable -ArgumentList $arguments -WorkingDirectory $runtimeRoot -WindowStyle Hidden -PassThru
