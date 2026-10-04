param([switch]$Validation,[switch]$GpuTest)
$ErrorActionPreference = 'Stop'
$project = Split-Path $PSScriptRoot -Parent
New-Item -ItemType Directory -Force -Path (Join-Path $project 'results') | Out-Null
& (Join-Path $PSScriptRoot 'prepare-vulkan.ps1') -Validation:$Validation
$toolsBuild = Join-Path $project 'build/vulkan-bootstrap'
& cmake -S $project -B $toolsBuild -G 'Visual Studio 17 2022' -A Win32 -DCOD4X1KHZ_BUILD_ENGINE=OFF -DCOD4X1KHZ_BUILD_VULKAN=ON -DCMAKE_BUILD_TYPE=Release
if($LASTEXITCODE) { throw 'Vulkan tool configuration failed' }
& cmake --build $toolsBuild --config Release --target khz_vulkan_probe khz_vulkan_shader_inventory khz_vulkan_shader_link khz_vulkan_shader_tests khz_vulkan_gpu_tests khz_vulkan_legacy_gpu_tests khz_vulkan_legacy --parallel 4
if($LASTEXITCODE) { throw 'Vulkan tool build failed' }
& ctest --test-dir $toolsBuild -C Release -R '^khz_vulkan_shader$' --output-on-failure
if($LASTEXITCODE) { throw 'Shader tests failed' }
if($Validation) {
    $layerBuild = Join-Path $project 'build/vulkan-validation'
    & cmake -S (Join-Path $project 'cmake/vulkan-validation') -B $layerBuild -G 'Visual Studio 17 2022' -A Win32 -DCMAKE_BUILD_TYPE=Release
    if($LASTEXITCODE) { throw 'Validation layer configuration failed' }
    & cmake --build $layerBuild --config Release --target khz_vulkan_validation --parallel 3
    if($LASTEXITCODE) { throw 'Win32 validation layer build failed' }
}
if($GpuTest) {
    $savedLayer = $env:VK_LAYER_PATH
    try {
        $testArguments = @('--output',(Join-Path $project 'results/vulkan-gpu-tools'))
        if($Validation) { $env:VK_LAYER_PATH = Join-Path $project '.deps/vulkan/layers/Release'; $testArguments += '--validation' }
        & (Join-Path $toolsBuild 'Release/khz_vulkan_gpu_tests.exe') @testArguments
        if($LASTEXITCODE) { throw 'Native Vulkan GPU fixtures failed' }
        if($Validation) {
            & (Join-Path $toolsBuild 'Release/khz_vulkan_legacy_gpu_tests.exe')
            if($LASTEXITCODE) { throw 'Native legacy graphics boundary fixtures failed' }
        }
    } finally { $env:VK_LAYER_PATH = $savedLayer }
}
