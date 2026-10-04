param([switch]$Validation,[switch]$GpuTest,
    [ValidateSet('Win32','x64')][string]$Architecture = 'Win32')
$ErrorActionPreference = 'Stop'
$project = Split-Path $PSScriptRoot -Parent
New-Item -ItemType Directory -Force -Path (Join-Path $project 'results') | Out-Null
& (Join-Path $PSScriptRoot 'prepare-vulkan.ps1') -Validation:$Validation
$buildDirectory = if ($Architecture -eq 'x64') { 'build/x64/vulkan-bootstrap' } else { 'build/vulkan-bootstrap' }
$toolsBuild = Join-Path $project $buildDirectory
& cmake -S $project -B $toolsBuild -G 'Visual Studio 17 2022' -A $Architecture -DCOD4X1KHZ_BUILD_ENGINE=OFF -DCOD4X1KHZ_BUILD_VULKAN=ON -DCMAKE_BUILD_TYPE=Release
if($LASTEXITCODE) { throw 'Vulkan tool configuration failed' }
& cmake --build $toolsBuild --config Release --target khz_vulkan_probe khz_vulkan_shader_inventory khz_vulkan_shader_link khz_vulkan_shader_tests khz_vulkan_gpu_tests khz_vulkan_legacy_gpu_tests khz_vulkan_legacy --parallel 4
if($LASTEXITCODE) { throw 'Vulkan tool build failed' }
& ctest --test-dir $toolsBuild -C Release -R '^khz_vulkan_shader$' --output-on-failure
if($LASTEXITCODE) { throw 'Shader tests failed' }
if($Validation) {
    $layerDirectory = if ($Architecture -eq 'x64') { 'build/x64/vulkan-validation' } else { 'build/vulkan-validation' }
    $layerBuild = Join-Path $project $layerDirectory
    & cmake -S (Join-Path $project 'cmake/vulkan-validation') -B $layerBuild -G 'Visual Studio 17 2022' -A $Architecture -DCMAKE_BUILD_TYPE=Release
    if($LASTEXITCODE) { throw 'Validation layer configuration failed' }
    & cmake --build $layerBuild --config Release --target khz_vulkan_validation --parallel 3
    if($LASTEXITCODE) { throw "$Architecture validation layer build failed" }
}
if($GpuTest) {
    $savedLayer = $env:VK_LAYER_PATH
    try {
        $outputDirectory = if ($Architecture -eq 'x64') { 'results/vulkan-gpu-tools-x64' } else { 'results/vulkan-gpu-tools' }
        $testArguments = @('--output',(Join-Path $project $outputDirectory))
        $layerOutput = if ($Architecture -eq 'x64') { '.deps/vulkan/layers/x64/Release' } else { '.deps/vulkan/layers/Release' }
        if($Validation) { $env:VK_LAYER_PATH = Join-Path $project $layerOutput; $testArguments += '--validation' }
        & (Join-Path $toolsBuild 'Release/khz_vulkan_gpu_tests.exe') @testArguments
        if($LASTEXITCODE) { throw 'Native Vulkan GPU fixtures failed' }
        if($Validation) {
            & (Join-Path $toolsBuild 'Release/khz_vulkan_legacy_gpu_tests.exe')
            if($LASTEXITCODE) { throw 'Native legacy graphics boundary fixtures failed' }
        }
    } finally { $env:VK_LAYER_PATH = $savedLayer }
}
