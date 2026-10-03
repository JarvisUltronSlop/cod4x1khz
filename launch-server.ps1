param([ValidateSet('mp_killhouse','mp_crash')][string]$Map = 'mp_killhouse', [ValidateRange(1024,65535)][int]$Port = 28960, [ValidateRange(0,15)][int]$BotCount = 15, [ValidateRange(0,3)][int]$BotDifficulty = 1, [switch]$Benchmark, [switch]$ScriptTests, [switch]$TestControls, [string]$Configuration = 'Release')
$ErrorActionPreference = 'Stop'
$runtimeRoot = Join-Path $PSScriptRoot 'runtime'
$executable = Join-Path $runtimeRoot 'cod4x1khz_server.exe'
if (!(Test-Path -LiteralPath $executable)) { throw 'Build the server with build.ps1 first' }
# khz_bots is latched and script bootstrap is selected at initialization.
# A later exec cannot enable it for the already initialized first map.
$arguments = @('+set','dedicated','1','+set','net_ip','127.0.0.1','+set','net_port',"$Port",'+set','khz_bots','1','+set','khz_bot_count',"$BotCount",'+set','khz_bot_difficulty',"$BotDifficulty",'+set','sv_maxclients','16','+exec','server.cfg')
if ($Benchmark -or $ScriptTests -or $TestControls) { $arguments += @('+set','khz_benchmark','1') }
if ($ScriptTests) { $arguments += @('+set','khz_script_tests','1') }
if ($TestControls) { $arguments += @('+set','khz_test_controls','1') }
$arguments += @('+set','khz_bot_count',"$BotCount",'+set','khz_bot_difficulty',"$BotDifficulty",'+map',$Map)
Start-Process -FilePath $executable -ArgumentList $arguments -WorkingDirectory $runtimeRoot -WindowStyle Hidden -PassThru
