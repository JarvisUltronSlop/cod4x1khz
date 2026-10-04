param([switch]$Validation)
$ErrorActionPreference = 'Stop'
$prepareArguments = @((Join-Path $PSScriptRoot 'prepare_vulkan.py'))
if($Validation) { $prepareArguments += '--validation' }
& python @prepareArguments
if ($LASTEXITCODE) { throw 'Pinned Vulkan dependency preparation failed' }
