cmake_minimum_required(VERSION 3.25)
project(khz_engine_port_audit LANGUAGES C CXX RC)
include(CTest)
get_filename_component(KHZ_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(SRC_DIR "${KHZ_ROOT}/engine/src")
set(DEPS_DIR "${KHZ_ROOT}/engine/deps")
set(SCRIPTS_DIR "${KHZ_ROOT}/engine/scripts")
set(KISAK_PLATFORM win32)
option(KISAK_OPENAL "Compile the native OpenAL backend" OFF)
include("${SCRIPTS_DIR}/platform_override.cmake")
include("${SCRIPTS_DIR}/common_files.cmake")
include("${SCRIPTS_DIR}/mp/mp_files.cmake")
include("${SCRIPTS_DIR}/platform/win32/platform.cmake")
set(KHZ_SOURCES)
foreach(group SRCROOT DYNENTITY EFFECTSCORE AIM_ASSIST BGAME BINKLIB CGAME CGAME_MP
    CLIENT CLIENT_MP COMMON DATABASE DEVGUI GAME GAME_MP GFX_D3D GROUPVOICE
    GROUPVOICE_SPEEX MSSLIB ODE PHYSICS PHYSICS_ODE QCOMMON RAGDOLL SCRIPT SERVER
    SERVER_MP SOUND SOUND_BACKEND SPEEX STRINGED UI UI_MP UNIVERSAL WIN32_SRC XANIM ZLIB STEAM_HEADERS)
  list(APPEND KHZ_SOURCES ${${group}})
endforeach()
file(READ "${KHZ_ROOT}/CMakeLists.txt" KHZ_MAIN_CMAKE)
string(REGEX MATCHALL "engine/src/khz/[A-Za-z0-9_]+\\.c(pp)?" KHZ_EXTENSION_PATHS "${KHZ_MAIN_CMAKE}")
set(KHZ_EXTENSION_SOURCES)
foreach(path ${KHZ_EXTENSION_PATHS})
  list(APPEND KHZ_EXTENSION_SOURCES "${KHZ_ROOT}/${path}")
endforeach()
list(REMOVE_DUPLICATES KHZ_EXTENSION_SOURCES)
# Compilation audit only. An archive cannot be run or deployed as a game and
# does not establish successful linking against native external dependencies.
add_library(khz_engine_port_audit STATIC ${KHZ_SOURCES} ${KHZ_EXTENSION_SOURCES})
target_include_directories(khz_engine_port_audit PRIVATE "${SRC_DIR}" "${DEPS_DIR}"
  "${KHZ_ROOT}/native" "${KHZ_ROOT}/sdk/include" "${DEPS_DIR}/msslib"
  "${KHZ_ROOT}/.deps/dxsdk/9.29.952.8/build/native/include"
  "${KHZ_ROOT}/.deps/vulkan/headers/include" "${KHZ_ROOT}/.deps/vulkan/allocator/include")
target_compile_definitions(khz_engine_port_audit PRIVATE KISAK_MP=1 COD4X1KHZ=1
  COD4X1KHZ_NATIVE_VULKAN=1 CINEMA USE_SEPARATE_BLIT_TEXTURE WIN32 _CONSOLE _MBCS
  _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_DEPRECATE)
target_compile_options(khz_engine_port_audit PRIVATE /W3 /fp:precise /Zi)
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
  target_compile_definitions(khz_engine_port_audit PRIVATE STEAM_API_NODLL=1)
  add_executable(khz_steam_native_runtime_tests "${KHZ_ROOT}/tests/steam_native_runtime_tests.cpp"
    "${SRC_DIR}/win32/win_steam_native.cpp")
  target_include_directories(khz_steam_native_runtime_tests PRIVATE "${DEPS_DIR}")
  target_compile_definitions(khz_steam_native_runtime_tests PRIVATE STEAM_API_NODLL=1)
  set_target_properties(khz_steam_native_runtime_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/steam-loader-fixture/$<CONFIG>")
  add_test(NAME khz_steam_native_runtime COMMAND khz_steam_native_runtime_tests)
endif()

option(KHZ_BUILD_NATIVE_CLIENT "Build the isolated native graphics client" OFF)
option(KHZ_NATIVE_CLIENT_VULKAN "Include the native Vulkan renderer in the isolated client" OFF)
option(KHZ_TEST_NATIVE_VIDEO "Build the native cinematic decoder fixture" OFF)
if(KHZ_BUILD_NATIVE_CLIENT OR KHZ_TEST_NATIVE_VIDEO)
  if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8 OR NOT KISAK_OPENAL)
    message(FATAL_ERROR "Native client/video integration requires x64 and OpenAL")
  endif()
  include("${KHZ_ROOT}/cmake/native-video.cmake")
endif()
if(KHZ_TEST_NATIVE_VIDEO)
  add_executable(khz_native_video_tests "${KHZ_ROOT}/tests/video_native_tests.cpp")
  target_include_directories(khz_native_video_tests PRIVATE "${DEPS_DIR}")
  target_compile_options(khz_native_video_tests PRIVATE /UNDEBUG)
  target_link_libraries(khz_native_video_tests PRIVATE khz_native_video)
  khz_stage_native_video(khz_native_video_tests)
  add_test(NAME khz_native_video COMMAND khz_native_video_tests "${KHZ_ROOT}/runtime/main/video/default.bik"
    "${KHZ_ROOT}/results/native-video-reference.yuv" "${KHZ_ROOT}/runtime/main/video/cod_intro.bik")
endif()
if(KHZ_BUILD_NATIVE_CLIENT)
  add_executable(khz_native_client WIN32 ${KHZ_SOURCES} ${KHZ_EXTENSION_SOURCES}
    "${KHZ_ROOT}/native/clock.cpp" "${SRC_DIR}/gfx/gfx_hash.cpp")
  target_include_directories(khz_native_client PRIVATE "${SRC_DIR}" "${DEPS_DIR}"
    "${KHZ_ROOT}/native" "${KHZ_ROOT}/sdk/include"
    "${KHZ_ROOT}/.deps/dxsdk/9.29.952.8/build/native/include")
  target_compile_definitions(khz_native_client PRIVATE KISAK_MP=1 CINEMA USE_SEPARATE_BLIT_TEXTURE
    COD4X1KHZ=1 COD4X1KHZ_ISOLATED_BUILD=1 KISAK_OPENAL=1 STEAM_API_NODLL=1 WIN32 _CONSOLE _MBCS
    _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_DEPRECATE)
  target_compile_options(khz_native_client PRIVATE /W3 /fp:precise /Zi /Gy /we4700)
  target_link_options(khz_native_client PRIVATE /DEBUG /OPT:REF /OPT:ICF /MACHINE:X64)
  target_link_libraries(khz_native_client PRIVATE khz_native_video OpenAL::OpenAL dbghelp winhttp bcrypt
    ws2_32 iphlpapi dsound d3d9 ddraw winmm kernel32 user32 gdi32 winspool comdlg32 advapi32
    shell32 ole32 oleaut32 uuid odbc32 odbccp32 dxguid
    "${KHZ_ROOT}/.deps/dxsdk/9.29.952.8/build/native/Release/lib/x64/d3dx9.lib")
  set_target_properties(khz_native_client PROPERTIES OUTPUT_NAME cod4x1khz_x64 ENABLE_EXPORTS ON
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/native-client/$<CONFIG>")
  if(KHZ_NATIVE_CLIENT_VULKAN)
    set(KHZ_TOOL_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/native-tools/$<CONFIG>")
    set(KHZ_D3DX9_LIBRARY "${KHZ_ROOT}/.deps/dxsdk/9.29.952.8/build/native/Release/lib/x64/d3dx9.lib")
    set(KHZ_D3DX9_RUNTIME "${KHZ_ROOT}/.deps/dxsdk/9.29.952.8/build/native/Release/bin/x64/D3DX9_43.dll")
    include("${KHZ_ROOT}/cmake/vulkan.cmake")
    target_compile_definitions(khz_native_client PRIVATE COD4X1KHZ_NATIVE_VULKAN=1)
    target_link_libraries(khz_native_client PRIVATE khz_vulkan_legacy)
  endif()
  khz_stage_native_video(khz_native_client)
  add_custom_command(TARGET khz_native_client POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${KHZ_ROOT}/.deps/dxsdk/9.29.952.8/build/native/Release/bin/x64/D3DX9_43.dll" "$<TARGET_FILE_DIR:khz_native_client>/D3DX9_43.dll")
endif()

if(KISAK_OPENAL)
  target_compile_definitions(khz_engine_port_audit PRIVATE KISAK_OPENAL=1)
  set_source_files_properties(${SRC_DIR}/sound/snd_driver_load_obj.cpp PROPERTIES
    COMPILE_DEFINITIONS "DR_WAV_IMPLEMENTATION;DR_MP3_IMPLEMENTATION")
  include("${SCRIPTS_DIR}/extern/openal.cmake")
  include(CTest)
  add_executable(khz_sound_openal_native_tests "${KHZ_ROOT}/tests/sound_openal_native_tests.cpp"
    "${SRC_DIR}/sound/snd_openal_runtime.cpp"
    "${SRC_DIR}/sound/snd_driver_load_obj.cpp")
  target_include_directories(khz_sound_openal_native_tests PRIVATE "${SRC_DIR}" "${DEPS_DIR}"
    "${KHZ_ROOT}/.deps/dxsdk/9.29.952.8/build/native/include")
  target_compile_definitions(khz_sound_openal_native_tests PRIVATE KISAK_MP=1 KISAK_OPENAL=1
    RELEASE_ASSERTS=1 _CRT_SECURE_NO_WARNINGS)
  target_compile_options(khz_sound_openal_native_tests PRIVATE /Gy /fp:precise)
  target_link_options(khz_sound_openal_native_tests PRIVATE /OPT:REF)
  target_link_libraries(khz_sound_openal_native_tests PRIVATE OpenAL::OpenAL)
  add_test(NAME khz_sound_openal_native COMMAND khz_sound_openal_native_tests)
endif()

# An isolated executable for progressing native server integration. This never
# participates in the application's deployment target or stages legacy DLLs.
option(KHZ_BUILD_NATIVE_DEDICATED "Build the experimental native dedicated server" OFF)
if(KHZ_BUILD_NATIVE_DEDICATED)
  if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8 OR NOT KISAK_OPENAL)
    message(FATAL_ERROR "The isolated native server requires x64 and KISAK_OPENAL=ON")
  endif()
  set(KHZ_DEDI_SOURCES ${KHZ_SOURCES})
  set(KHZ_DEDI_EXTENSION_SOURCES ${KHZ_EXTENSION_SOURCES})
  list(FILTER KHZ_DEDI_EXTENSION_SOURCES EXCLUDE REGEX "khz_(render_profile|frontend_upload|shader_inventory)\\.cpp$")
  # The production server does not define CINEMA and never decodes video.
  list(FILTER KHZ_DEDI_SOURCES EXCLUDE REGEX "binktextures\\.cpp$")
  add_executable(khz_native_dedicated WIN32 ${KHZ_DEDI_SOURCES} ${KHZ_DEDI_EXTENSION_SOURCES}
    "${KHZ_ROOT}/native/clock.cpp" "${SRC_DIR}/gfx/gfx_hash.cpp")
  target_include_directories(khz_native_dedicated PRIVATE "${SRC_DIR}" "${DEPS_DIR}"
    "${KHZ_ROOT}/native" "${KHZ_ROOT}/sdk/include"
    "${KHZ_ROOT}/.deps/dxsdk/9.29.952.8/build/native/include")
  target_compile_definitions(khz_native_dedicated PRIVATE KISAK_MP=1 KISAK_DEDICATED=1 DEDICATED=1
    COD4X1KHZ=1 COD4X1KHZ_ISOLATED_BUILD=1 KISAK_OPENAL=1 STEAM_API_NODLL=1 WIN32 _CONSOLE _MBCS
    _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_DEPRECATE)
  target_compile_options(khz_native_dedicated PRIVATE /W3 /fp:precise /Zi /Gy /we4700)
  target_link_options(khz_native_dedicated PRIVATE /DEBUG /OPT:REF /OPT:ICF /MACHINE:X64)
  target_link_libraries(khz_native_dedicated PRIVATE OpenAL::OpenAL dbghelp winhttp bcrypt
    ws2_32 iphlpapi dsound d3d9 ddraw winmm kernel32 user32 gdi32 winspool comdlg32 advapi32
    shell32 ole32 oleaut32 uuid odbc32 odbccp32 dxguid
    "${KHZ_ROOT}/.deps/dxsdk/9.29.952.8/build/native/Release/lib/x64/d3dx9.lib")
  set_target_properties(khz_native_dedicated PROPERTIES OUTPUT_NAME cod4x1khz_server_x64
    ENABLE_EXPORTS ON
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/native-server/$<CONFIG>")
endif()
