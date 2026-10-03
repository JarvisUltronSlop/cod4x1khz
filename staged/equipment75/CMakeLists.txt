cmake_minimum_required(VERSION 3.25)
project(khz_equipment_proposal LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")
include(CTest)
add_executable(equipment_policy_test tests/equipment_policy_test.cpp)
target_include_directories(equipment_policy_test PRIVATE native)
target_compile_options(equipment_policy_test PRIVATE /W4 /UNDEBUG)
add_test(NAME equipment_policy COMMAND equipment_policy_test)

# Compile-only adapters against the actual current native headers. They do not
# link/deploy an engine and must not be treated as live endpoint evidence.
get_filename_component(PROJECT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
foreach(kind IN ITEMS client server)
  add_library(equipment_${kind}_typecheck OBJECT engine/src/khz/khz_equipment_inspection.cpp)
  target_include_directories(equipment_${kind}_typecheck PRIVATE native engine/src/khz
    "${PROJECT_ROOT}/native" "${PROJECT_ROOT}/engine/src" "${PROJECT_ROOT}/engine/src/khz" "${PROJECT_ROOT}/engine/deps"
    "${PROJECT_ROOT}/.deps/dxsdk/9.29.952.8/build/native/include")
  target_compile_definitions(equipment_${kind}_typecheck PRIVATE KISAK_MP COD4X1KHZ=1)
  if(kind STREQUAL "server")
    target_compile_definitions(equipment_${kind}_typecheck PRIVATE KISAK_DEDICATED DEDICATED)
  endif()
  target_compile_options(equipment_${kind}_typecheck PRIVATE /W3 /fp:strict)
endforeach()
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/typecheck/profile.cpp")
  foreach(kind IN ITEMS owner profile)
    add_library(equipment_${kind}_fragment_typecheck OBJECT "typecheck/${kind}.cpp")
    target_include_directories(equipment_${kind}_fragment_typecheck PRIVATE native engine/src/khz
      "${PROJECT_ROOT}/native" "${PROJECT_ROOT}/engine/src" "${PROJECT_ROOT}/engine/src/khz" "${PROJECT_ROOT}/engine/deps"
      "${PROJECT_ROOT}/.deps/dxsdk/9.29.952.8/build/native/include")
    target_compile_definitions(equipment_${kind}_fragment_typecheck PRIVATE KISAK_MP COD4X1KHZ=1)
    target_compile_options(equipment_${kind}_fragment_typecheck PRIVATE /W3 /fp:strict)
  endforeach()
endif()
