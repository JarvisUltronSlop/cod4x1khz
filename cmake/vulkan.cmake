get_filename_component(KHZ_VULKAN_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(KHZ_VULKAN_DEPS "${KHZ_VULKAN_ROOT}/.deps/vulkan")
if(NOT EXISTS "${KHZ_VULKAN_DEPS}/headers/include/vulkan/vulkan.h")
  message(FATAL_ERROR "Run scripts/prepare-vulkan.ps1 for pinned project-local Vulkan sources")
endif()
add_library(khz_vulkan_context STATIC "${KHZ_VULKAN_ROOT}/engine/src/gfx_vulkan/vk_context.cpp")
target_include_directories(khz_vulkan_context PUBLIC
  "${KHZ_VULKAN_ROOT}/engine/src"
  "${KHZ_VULKAN_DEPS}/headers/include"
  "${KHZ_VULKAN_DEPS}/allocator/include")
target_compile_options(khz_vulkan_context PRIVATE /W3 /fp:precise)
target_compile_definitions(khz_vulkan_context PUBLIC VMA_STATIC_VULKAN_FUNCTIONS=0 VMA_DYNAMIC_VULKAN_FUNCTIONS=1)
add_executable(khz_vulkan_probe "${KHZ_VULKAN_ROOT}/native/vulkan_probe.cpp")
add_executable(khz_shader_disassemble "${KHZ_VULKAN_ROOT}/native/shader_disassemble.cpp")
target_link_libraries(khz_shader_disassemble PRIVATE d3dcompiler)
target_link_libraries(khz_vulkan_probe PRIVATE khz_vulkan_context)
set_target_properties(khz_vulkan_probe PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${KHZ_TOOL_OUTPUT_DIR}")

# Compile only the bytecode parser and Vulkan emitter. The upstream CMake file
# generates into its source directory and pulls in unrelated API backends.
set(KHZ_MOJO "${KHZ_VULKAN_DEPS}/mojoshader")
add_library(khz_mojoshader STATIC
  "${KHZ_MOJO}/mojoshader.c" "${KHZ_VULKAN_ROOT}/engine/src/gfx_vulkan/mojo_common.c"
  "${KHZ_MOJO}/profiles/mojoshader_profile_common.c" "${KHZ_MOJO}/profiles/mojoshader_profile_spirv.c")
target_include_directories(khz_mojoshader PUBLIC "${KHZ_MOJO}")
target_compile_definitions(khz_mojoshader PUBLIC MOJOSHADER_NO_VERSION_INCLUDE=1)
target_compile_definitions(khz_mojoshader PRIVATE _CRT_SECURE_NO_WARNINGS
  SUPPORT_PROFILE_SPIRV=1 SUPPORT_PROFILE_GLSPIRV=0 SUPPORT_PROFILE_D3D=0
  SUPPORT_PROFILE_BYTECODE=0 SUPPORT_PROFILE_HLSL=0 SUPPORT_PROFILE_GLSL=0
  SUPPORT_PROFILE_GLSL120=0 SUPPORT_PROFILE_GLSLES=0 SUPPORT_PROFILE_GLSLES3=0
  SUPPORT_PROFILE_ARB1=0 SUPPORT_PROFILE_ARB1_NV=0 SUPPORT_PROFILE_METAL=0)
set(SPIRV-Headers_SOURCE_DIR "${KHZ_VULKAN_DEPS}/spirv-headers")
set(SPIRV_SKIP_TESTS ON CACHE BOOL "" FORCE)
set(SPIRV_SKIP_EXECUTABLES ON CACHE BOOL "" FORCE)
set(SPIRV_TOOLS_BUILD_STATIC ON CACHE BOOL "" FORCE)
set(SPIRV_WERROR OFF CACHE BOOL "" FORCE)
set(ENABLE_SPIRV_TOOLS_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory("${KHZ_VULKAN_DEPS}/spirv-tools" "${CMAKE_BINARY_DIR}/vulkan/spirv-tools" EXCLUDE_FROM_ALL)
add_library(khz_vulkan_shader STATIC "${KHZ_VULKAN_ROOT}/engine/src/gfx_vulkan/vk_shader.cpp" "${KHZ_VULKAN_ROOT}/engine/src/gfx_vulkan/vk_alpha.cpp" "${KHZ_VULKAN_ROOT}/engine/src/gfx_vulkan/vk_depth_sampling.cpp" "${KHZ_VULKAN_ROOT}/engine/src/gfx_vulkan/vk_shader_cache.cpp" "${KHZ_VULKAN_ROOT}/engine/src/gfx/gfx_hash.cpp")
target_include_directories(khz_vulkan_shader PRIVATE "${KHZ_VULKAN_DEPS}/spirv-headers/include")
target_link_libraries(khz_vulkan_shader PUBLIC khz_vulkan_context khz_mojoshader SPIRV-Tools-static bcrypt)
add_executable(khz_vulkan_shader_inventory "${KHZ_VULKAN_ROOT}/native/vulkan_shader_inventory.cpp")
target_link_libraries(khz_vulkan_shader_inventory PRIVATE khz_vulkan_shader)
set_target_properties(khz_vulkan_shader_inventory PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${KHZ_TOOL_OUTPUT_DIR}")
add_executable(khz_vulkan_shader_link "${KHZ_VULKAN_ROOT}/native/vulkan_shader_link.cpp")
target_link_libraries(khz_vulkan_shader_link PRIVATE khz_vulkan_shader)
set_target_properties(khz_vulkan_shader_link PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${KHZ_TOOL_OUTPUT_DIR}")
add_library(khz_vulkan_backend STATIC "${KHZ_VULKAN_ROOT}/engine/src/gfx_vulkan/vk_backend.cpp")
target_include_directories(khz_vulkan_backend PUBLIC "${KHZ_VULKAN_ROOT}/native")
target_link_libraries(khz_vulkan_backend PUBLIC khz_vulkan_shader)
add_library(khz_graphics_gamma STATIC "${KHZ_VULKAN_ROOT}/engine/src/gfx/gamma_pass.cpp")
target_include_directories(khz_graphics_gamma PUBLIC "${KHZ_VULKAN_ROOT}/native" "${KHZ_VULKAN_ROOT}/engine/src")
target_link_libraries(khz_graphics_gamma PUBLIC d3dcompiler)
add_library(khz_d3d9_backend STATIC "${KHZ_VULKAN_ROOT}/engine/src/gfx_d3d/d3d9_backend.cpp")
add_library(khz_legacy_resources STATIC "${KHZ_VULKAN_ROOT}/engine/src/gfx/legacy_resources.cpp")
target_include_directories(khz_legacy_resources PUBLIC "${KHZ_VULKAN_ROOT}/native" "${KHZ_VULKAN_ROOT}/engine/src")
add_library(khz_vulkan_legacy STATIC "${KHZ_VULKAN_ROOT}/engine/src/gfx_vulkan/vk_legacy_device.cpp")
target_include_directories(khz_vulkan_legacy PRIVATE "${KHZ_VULKAN_ROOT}/.deps/dxsdk/9.29.952.8/build/native/include")
target_link_libraries(khz_vulkan_legacy PUBLIC khz_legacy_resources khz_vulkan_backend khz_d3d9_backend khz_graphics_gamma d3d9
  "${KHZ_D3DX9_LIBRARY}")
add_custom_command(TARGET khz_vulkan_probe POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${KHZ_D3DX9_RUNTIME}" "$<TARGET_FILE_DIR:khz_vulkan_probe>/D3DX9_43.dll")
target_include_directories(khz_d3d9_backend PUBLIC "${KHZ_VULKAN_ROOT}/native" "${KHZ_VULKAN_ROOT}/engine/src")
target_link_libraries(khz_d3d9_backend PUBLIC d3d9)
if(BUILD_TESTING)
  add_executable(khz_vulkan_legacy_gpu_tests EXCLUDE_FROM_ALL "${KHZ_VULKAN_ROOT}/tests/vulkan_legacy_gpu_tests.cpp")
  target_compile_options(khz_vulkan_legacy_gpu_tests PRIVATE /UNDEBUG)
  target_link_libraries(khz_vulkan_legacy_gpu_tests PRIVATE khz_vulkan_legacy d3dcompiler)
  add_executable(khz_vulkan_gpu_tests EXCLUDE_FROM_ALL "${KHZ_VULKAN_ROOT}/tests/vulkan_gpu_tests.cpp")
  target_include_directories(khz_vulkan_gpu_tests PRIVATE "${KHZ_VULKAN_ROOT}/.deps/dxsdk/9.29.952.8/build/native/include")
  target_compile_options(khz_vulkan_gpu_tests PRIVATE /UNDEBUG)
  target_link_libraries(khz_vulkan_gpu_tests PRIVATE khz_vulkan_backend khz_d3d9_backend khz_graphics_gamma d3dcompiler)
  target_link_libraries(khz_vulkan_gpu_tests PRIVATE "${KHZ_D3DX9_LIBRARY}")
  foreach(gpu_target khz_vulkan_gpu_tests khz_vulkan_legacy_gpu_tests)
    add_custom_command(TARGET ${gpu_target} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${KHZ_D3DX9_RUNTIME}" "$<TARGET_FILE_DIR:${gpu_target}>/D3DX9_43.dll")
  endforeach()
  add_executable(khz_vulkan_shader_tests "${KHZ_VULKAN_ROOT}/tests/vulkan_shader_tests.cpp")
  target_include_directories(khz_vulkan_shader_tests PRIVATE "${KHZ_VULKAN_ROOT}/native")
  target_compile_options(khz_vulkan_shader_tests PRIVATE /UNDEBUG)
  target_link_libraries(khz_vulkan_shader_tests PRIVATE khz_vulkan_shader d3dcompiler)
  add_test(NAME khz_vulkan_shader COMMAND khz_vulkan_shader_tests)
endif()

add_executable(khz_shader_register_probe EXCLUDE_FROM_ALL "${KHZ_VULKAN_ROOT}/native/shader_register_probe.cpp")
target_link_libraries(khz_shader_register_probe PRIVATE khz_vulkan_shader)
