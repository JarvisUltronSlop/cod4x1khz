cmake_minimum_required(VERSION 3.25)
project(khz_asset_layout_probe LANGUAGES CXX)
get_filename_component(KHZ_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
find_package(Python3 COMPONENTS Interpreter REQUIRED)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")
set(KHZ_PROBE_SOURCE "${CMAKE_CURRENT_BINARY_DIR}/asset-layout-probe.cpp")
set(KHZ_PROBE_HEADERS "${CMAKE_CURRENT_BINARY_DIR}/inspection-headers")
file(GLOB_RECURSE KHZ_HEADER_INPUTS CONFIGURE_DEPENDS "${KHZ_ROOT}/engine/src/*.h")
add_custom_command(OUTPUT "${KHZ_PROBE_SOURCE}"
  COMMAND "${Python3_EXECUTABLE}" "${KHZ_ROOT}/scripts/generate-asset-layout-probe.py" "${KHZ_PROBE_SOURCE}" --headers "${KHZ_PROBE_HEADERS}"
  DEPENDS "${KHZ_ROOT}/scripts/generate-asset-layout-probe.py" "${KHZ_ROOT}/engine/src/database/db_load.cpp" ${KHZ_HEADER_INPUTS}
  VERBATIM)
# This executable contains header definitions and metadata only. It is neither
# an engine target nor evidence that the native game compiles or runs.
add_executable(khz_asset_layout_probe "${KHZ_PROBE_SOURCE}")
target_include_directories(khz_asset_layout_probe PRIVATE "${KHZ_PROBE_HEADERS}" "${KHZ_ROOT}/engine/src"
  "${KHZ_ROOT}/engine/deps" "${KHZ_ROOT}/native"
  "${KHZ_ROOT}/.deps/dxsdk/9.29.952.8/build/native/include")
target_compile_definitions(khz_asset_layout_probe PRIVATE KISAK_MP=1 _CRT_SECURE_NO_WARNINGS)
option(KHZ_VERIFY_ASSET_SCHEMAS "Verify generated converter offsets against the current compiler ABI" OFF)
if(KHZ_VERIFY_ASSET_SCHEMAS)
  target_compile_definitions(khz_asset_layout_probe PRIVATE KHZ_VERIFY_ASSET_SCHEMAS=1)
endif()
target_compile_options(khz_asset_layout_probe PRIVATE /Zi /Od)
target_link_options(khz_asset_layout_probe PRIVATE /DEBUG /OPT:NOREF /OPT:NOICF)
