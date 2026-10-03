# Camera-verified diagnostics: four eight-second cap stages and physical walking.
param(
    [ValidateSet('mp_killhouse','mp_crash')][string]$Map = 'mp_killhouse',
    [ValidateSet('480p','720p')][string]$Resolution = '480p',
    [ValidateRange(1,100000)][int]$Build = 86,
    [switch]$Timing
)
$ErrorActionPreference = 'Stop'
$timingSeconds = if ($Timing) { 60 } else { 0 }
& python (Join-Path $PSScriptRoot 'scripts/native_render_scene_probe.py') --build $Build --map $Map --resolution $Resolution --timing-seconds $timingSeconds
if ($LASTEXITCODE) { throw 'Camera, gameplay, render-cap or owned-process cleanup checks failed; see the printed result path.' }
