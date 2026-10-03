cmake_minimum_required(VERSION 3.25)
project(khz_archive_staged_contracts LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_MSVC_RUNTIME_LIBRARY MultiThreaded)
add_executable(archive_policy_contracts archive_policy_tests.cpp)
target_compile_options(archive_policy_contracts PRIVATE /W4 /UNDEBUG /fp:precise)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/native_adapter_tests.cpp")
  add_executable(archive_adapter_contracts native_adapter_tests.cpp native_adapter.cpp)
  get_filename_component(project_root "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
  add_executable(archive_production_adapter_contracts native_adapter_tests.cpp
    "${project_root}/engine/src/khz/khz_archive.cpp")
  add_library(archive_native_probe_typecheck OBJECT native_probe_typecheck.cpp native_probe_profile_typecheck.cpp)
  target_compile_definitions(archive_production_adapter_contracts PRIVATE KHZ_PRODUCTION_ADAPTER_TEST=1)
  foreach(adapter_target archive_adapter_contracts archive_production_adapter_contracts archive_native_probe_typecheck)
  target_include_directories(${adapter_target} PRIVATE
    "${project_root}/engine/src" "${project_root}/native" "${project_root}/engine/deps"
    "${project_root}/engine/deps/msslib" "${project_root}/sdk/include"
    "${project_root}/.deps/dxsdk/9.29.952.8/build/native/include"
    "${project_root}/build/Release/_deps/tracy-src/public")
  target_compile_definitions(${adapter_target} PRIVATE KISAK_MP=1 KISAK_DEDICATED=1
    COD4X1KHZ=1 _CRT_SECURE_NO_WARNINGS WIN32 _WINDOWS RELEASE_ASSERTS=1)
  target_compile_options(${adapter_target} PRIVATE /W3 /UNDEBUG /fp:precise)
  endforeach()
endif()
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/production_integration_plan.json")
  file(READ "${CMAKE_CURRENT_SOURCE_DIR}/production_integration_plan.json" integration_plan)
  string(JSON prepared_count LENGTH "${integration_plan}" prepared_cpp)
  math(EXPR prepared_last "${prepared_count} - 1")
  set(prepared_sources)
  foreach(i RANGE 0 ${prepared_last})
    string(JSON relative_source GET "${integration_plan}" prepared_cpp ${i})
    set(proposed_source "${CMAKE_CURRENT_SOURCE_DIR}/proposed-native/${relative_source}")
    list(APPEND prepared_sources "${proposed_source}")
    get_filename_component(original_directory "${project_root}/${relative_source}" DIRECTORY)
    set_source_files_properties("${proposed_source}" PROPERTIES INCLUDE_DIRECTORIES "${original_directory}")
  endforeach()
  add_library(archive_native_boundary_typecheck OBJECT ${prepared_sources})
  target_include_directories(archive_native_boundary_typecheck PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/proposed-native/engine/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/proposed-native/native"
    "${project_root}/engine/src" "${project_root}/engine/src/khz"
    "${project_root}/native" "${project_root}/engine/deps"
    "${project_root}/engine/deps/msslib" "${project_root}/sdk/include"
    "${project_root}/.deps/dxsdk/9.29.952.8/build/native/include"
    "${project_root}/build/Release/_deps/tracy-src/public")
  target_compile_definitions(archive_native_boundary_typecheck PRIVATE KISAK_MP=1 KISAK_DEDICATED=1
    COD4X1KHZ=1 _CRT_SECURE_NO_WARNINGS WIN32 _WINDOWS RELEASE_ASSERTS=1)
  target_compile_options(archive_native_boundary_typecheck PRIVATE /W3 /UNDEBUG /fp:precise)
endif()
