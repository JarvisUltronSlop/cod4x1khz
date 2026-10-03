cmake_minimum_required(VERSION 3.25)
project(cod4x1khz LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
option(COD4X1KHZ_BUILD_ENGINE "Build the multiplayer client and dedicated server" ON)
option(COD4X1KHZ_INSTRUMENT "Enable Tracy engine instrumentation" OFF)
include(CTest)
add_executable(khz_inspect native/inspect.cpp)
target_link_libraries(khz_inspect PRIVATE dbghelp)
target_compile_definitions(khz_inspect PRIVATE KISAK_MP COD4X1KHZ=1)
target_include_directories(khz_inspect PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/engine/src"
  "${CMAKE_CURRENT_SOURCE_DIR}/engine/deps" "${CMAKE_CURRENT_SOURCE_DIR}/native"
  "${CMAKE_CURRENT_SOURCE_DIR}/.deps/dxsdk/9.29.952.8/build/native/include")
add_executable(khz_launch_probe EXCLUDE_FROM_ALL native/launch_probe.cpp)
target_link_libraries(khz_launch_probe PRIVATE dbghelp)
add_executable(khz_audio_probe EXCLUDE_FROM_ALL native/audio_probe.cpp)
target_sources(khz_audio_probe PRIVATE native/archive_io_bridge.cpp engine/src/qcommon/unzip.cpp
  engine/deps/zlib/adler32.c engine/deps/zlib/crc32.c engine/deps/zlib/infblock.c
  engine/deps/zlib/infcodes.c engine/deps/zlib/inffast.c engine/deps/zlib/inflate.c
  engine/deps/zlib/inftrees.c engine/deps/zlib/infutil.c engine/deps/zlib/zutil.c)
target_include_directories(khz_audio_probe PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/engine/src" "${CMAKE_CURRENT_SOURCE_DIR}/engine/deps")
target_include_directories(khz_audio_probe PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/engine/deps/msslib")
target_link_libraries(khz_audio_probe PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/engine/deps/msslib/mss32.lib")
set_target_properties(khz_audio_probe PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/runtime/tools")
add_custom_command(TARGET khz_audio_probe POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
  "${CMAKE_CURRENT_SOURCE_DIR}/runtime/mss32.dll" "$<TARGET_FILE_DIR:khz_audio_probe>/mss32.dll")
add_library(khz_core STATIC native/clock.cpp)
target_include_directories(khz_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/native")
target_compile_options(khz_core PRIVATE /W4 /fp:precise)
if(BUILD_TESTING)
  add_executable(khz_tests tests/core_tests.cpp)
  target_link_libraries(khz_tests PRIVATE khz_core)
  add_test(NAME khz_core COMMAND khz_tests)
  add_executable(khz_input_timestamp_tests tests/input_timestamp_tests.cpp)
  target_include_directories(khz_input_timestamp_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/native")
  add_test(NAME khz_input_timestamp COMMAND khz_input_timestamp_tests)
  add_executable(khz_host_checkpoint_policy_tests tests/host_checkpoint_policy_tests.cpp)
  target_include_directories(khz_host_checkpoint_policy_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/native")
  target_compile_features(khz_host_checkpoint_policy_tests PRIVATE cxx_std_17)
  target_compile_options(khz_host_checkpoint_policy_tests PRIVATE /UNDEBUG /W4)
  add_test(NAME khz_host_checkpoint_policy COMMAND khz_host_checkpoint_policy_tests)
  add_executable(khz_huffman_native_tests tests/huffman_native_tests.cpp engine/src/qcommon/huffman.cpp)
  target_include_directories(khz_huffman_native_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/native" "${CMAKE_CURRENT_SOURCE_DIR}/engine/src")
  target_compile_options(khz_huffman_native_tests PRIVATE /UNDEBUG /W4)
  target_compile_definitions(khz_huffman_native_tests PRIVATE KISAK_MP=1)
  find_package(Threads REQUIRED)
  target_link_libraries(khz_huffman_native_tests PRIVATE Threads::Threads)
  add_test(NAME khz_huffman_native COMMAND khz_huffman_native_tests)
  add_executable(khz_msg_bits_native_tests tests/msg_bits_native_tests.cpp)
  target_include_directories(khz_msg_bits_native_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/native" "${CMAKE_CURRENT_SOURCE_DIR}/engine/src")
  target_compile_definitions(khz_msg_bits_native_tests PRIVATE KISAK_MP=1)
  target_compile_options(khz_msg_bits_native_tests PRIVATE /UNDEBUG /W4)
  add_test(NAME khz_msg_bits_native COMMAND khz_msg_bits_native_tests "${CMAKE_CURRENT_SOURCE_DIR}/results/msg-bits-native-evidence.json")
  add_executable(khz_park_wait_tests tests/park_wait_tests.cpp)
  target_include_directories(khz_park_wait_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/native")
  target_compile_options(khz_park_wait_tests PRIVATE /UNDEBUG /W4)
  target_link_libraries(khz_park_wait_tests PRIVATE Threads::Threads)
  add_test(NAME khz_park_wait COMMAND khz_park_wait_tests)
  add_executable(khz_steam_identity_format_tests tests/steam_identity_format_tests.cpp)
  target_include_directories(khz_steam_identity_format_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/native")
  target_compile_options(khz_steam_identity_format_tests PRIVATE /UNDEBUG /W4)
  add_test(NAME khz_steam_identity_format COMMAND khz_steam_identity_format_tests)
  add_executable(khz_archive_history_tests tests/archive_history_tests.cpp)
  target_include_directories(khz_archive_history_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/native")
  target_compile_options(khz_archive_history_tests PRIVATE /UNDEBUG /W4)
  add_test(NAME khz_archive_history COMMAND khz_archive_history_tests)
  add_executable(khz_plugin_packet_mailbox_tests engine/tests/khz_plugin_packet_mailbox_tests.cpp)
  target_include_directories(khz_plugin_packet_mailbox_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/native")
  target_compile_options(khz_plugin_packet_mailbox_tests PRIVATE /UNDEBUG /W4)
  find_package(Threads REQUIRED)
  target_link_libraries(khz_plugin_packet_mailbox_tests PRIVATE Threads::Threads)
  add_test(NAME khz_plugin_packet_mailbox COMMAND khz_plugin_packet_mailbox_tests)
  add_executable(khz_publication_tests tests/publication_tests.cpp)
  target_include_directories(khz_publication_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/native")
  target_compile_options(khz_publication_tests PRIVATE /W4)
  add_test(NAME khz_publication COMMAND khz_publication_tests)
  add_executable(khz_snapshot_header_history_tests tests/snapshot_header_history_tests.cpp)
  target_include_directories(khz_snapshot_header_history_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/native")
  add_test(NAME khz_snapshot_header_history COMMAND khz_snapshot_header_history_tests)
  add_executable(khz_canonical_tests tests/canonical_tests.cpp)
  target_include_directories(khz_canonical_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/native")
  add_test(NAME khz_canonical COMMAND khz_canonical_tests)
  add_executable(khz_replay_capture_tests tests/replay_capture_tests.cpp)
  target_include_directories(khz_replay_capture_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/native")
  add_test(NAME khz_replay_capture COMMAND khz_replay_capture_tests)
  add_executable(khz_server_capture_tests tests/server_capture_tests.cpp)
  target_include_directories(khz_server_capture_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/native")
  add_test(NAME khz_server_capture COMMAND khz_server_capture_tests)
  add_executable(khz_server_checkpoint_tests tests/server_checkpoint_tests.cpp)
  target_include_directories(khz_server_checkpoint_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/native")
  target_compile_options(khz_server_checkpoint_tests PRIVATE /UNDEBUG /W4)
  add_test(NAME khz_server_checkpoint COMMAND khz_server_checkpoint_tests)
  add_executable(khz_plugin_net_address_tests engine/tests/khz_plugin_net_address_tests.cpp
    engine/tests/khz_public_net_address_smoke.c engine/src/khz/khz_plugin_net_address.cpp)
  target_include_directories(khz_plugin_net_address_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/sdk/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/engine/src")
  target_compile_options(khz_plugin_net_address_tests PRIVATE /UNDEBUG /W4)
  target_link_libraries(khz_plugin_net_address_tests PRIVATE ws2_32 iphlpapi)
  add_test(NAME khz_plugin_net_address COMMAND khz_plugin_net_address_tests)
  add_executable(khz_plugin_dns_tests engine/tests/khz_plugin_dns_tests.cpp
    engine/src/khz/khz_plugin_dns.cpp engine/src/khz/khz_plugin_net_address.cpp)
  target_include_directories(khz_plugin_dns_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/engine/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/sdk/include")
  target_compile_definitions(khz_plugin_dns_tests PRIVATE KHZ_PLUGIN_DNS_TESTS=1)
  target_link_libraries(khz_plugin_dns_tests PRIVATE ws2_32 iphlpapi)
  add_test(NAME khz_plugin_dns COMMAND khz_plugin_dns_tests)
  set_tests_properties(khz_plugin_dns PROPERTIES TIMEOUT 20)
  add_executable(khz_gameplay_rng_tests tests/gameplay_rng_tests.cpp)
  target_include_directories(khz_gameplay_rng_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/native")
  add_test(NAME khz_gameplay_rng COMMAND khz_gameplay_rng_tests)
  add_executable(khz_service_control_policy_tests tests/service_control_policy_tests.cpp)
  target_include_directories(khz_service_control_policy_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/native")
  add_test(NAME khz_service_control_policy COMMAND khz_service_control_policy_tests)
  add_executable(khz_input_canonical_tests tests/input_canonical_tests.cpp)
  target_include_directories(khz_input_canonical_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/native")
  add_test(NAME khz_input_canonical COMMAND khz_input_canonical_tests)
  add_executable(khz_client_checkpoint_tests tests/client_checkpoint_tests.cpp)
  target_include_directories(khz_client_checkpoint_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/native")
  target_compile_options(khz_client_checkpoint_tests PRIVATE /UNDEBUG /W4)
  add_test(NAME khz_client_checkpoint COMMAND khz_client_checkpoint_tests)
  add_executable(khz_input_replay_policy_tests tests/input_replay_policy_tests.cpp)
  target_include_directories(khz_input_replay_policy_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/native" "${CMAKE_CURRENT_SOURCE_DIR}/engine/src")
  add_test(NAME khz_input_replay_policy COMMAND khz_input_replay_policy_tests)
  add_executable(khz_connectionless_policy_tests tests/connectionless_policy_tests.cpp)
  target_include_directories(khz_connectionless_policy_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/native")
  add_test(NAME khz_connectionless_policy COMMAND khz_connectionless_policy_tests)
  add_executable(khz_command_arguments_tests tests/command_arguments_tests.cpp)
  target_include_directories(khz_command_arguments_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/native")
  add_test(NAME khz_command_arguments COMMAND khz_command_arguments_tests)
  add_executable(khz_snapshot_reader_window_tests tests/snapshot_reader_window_tests.cpp)
  target_include_directories(khz_snapshot_reader_window_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/native")
  add_test(NAME khz_snapshot_reader_window COMMAND khz_snapshot_reader_window_tests)
  add_executable(khz_identity_admin_tests tests/identity_admin_tests.cpp
    engine/src/khz/khz_identity.cpp engine/src/khz/khz_admin.cpp)
  target_include_directories(khz_identity_admin_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/engine/src/khz")
  add_test(NAME khz_identity_admin COMMAND khz_identity_admin_tests)
  add_executable(khz_input_owner_tests engine/tests/khz_input_owner_tests.cpp)
  target_include_directories(khz_input_owner_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/engine/src")
  add_test(NAME khz_input_owner COMMAND khz_input_owner_tests)
  add_executable(khz_plugin_registry_tests tests/plugin_registry_tests.cpp)
  target_include_directories(khz_plugin_registry_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/native")
  add_test(NAME khz_plugin_registry COMMAND khz_plugin_registry_tests)
  add_executable(khz_plugin_io_policy_tests tests/plugin_io_policy_tests.cpp)
  target_include_directories(khz_plugin_io_policy_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/native")
  add_test(NAME khz_plugin_io_policy COMMAND khz_plugin_io_policy_tests)
  add_executable(khz_plugin_http_tests engine/tests/khz_plugin_http_tests.cpp
    engine/src/khz/khz_plugin_http.cpp)
  target_include_directories(khz_plugin_http_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/engine/src" "${CMAKE_CURRENT_SOURCE_DIR}/native")
  target_link_libraries(khz_plugin_http_tests PRIVATE winhttp)
  add_test(NAME khz_plugin_http COMMAND khz_plugin_http_tests)
  add_executable(khz_plugin_tcp_tests engine/tests/khz_plugin_tcp_tests.cpp
    engine/src/khz/khz_plugin_tcp.cpp)
  target_include_directories(khz_plugin_tcp_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/engine/src" "${CMAKE_CURRENT_SOURCE_DIR}/native")
  target_compile_definitions(khz_plugin_tcp_tests PRIVATE KHZ_PLUGIN_TCP_TESTS=1)
  target_link_libraries(khz_plugin_tcp_tests PRIVATE ws2_32)
  add_test(NAME khz_plugin_tcp COMMAND khz_plugin_tcp_tests)
  add_executable(khz_plugin_fs_tests engine/tests/khz_plugin_fs_tests.cpp
    engine/src/khz/khz_plugin_fs.cpp engine/src/khz/khz_plugin_unzip.c
    engine/src/khz/khz_content.cpp
    engine/deps/zlib/adler32.c engine/deps/zlib/crc32.c engine/deps/zlib/infblock.c
    engine/deps/zlib/infcodes.c engine/deps/zlib/inffast.c engine/deps/zlib/inflate.c
    engine/deps/zlib/inftrees.c engine/deps/zlib/infutil.c engine/deps/zlib/zutil.c)
  target_include_directories(khz_plugin_fs_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/engine/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/engine/deps/zlib" "${CMAKE_CURRENT_SOURCE_DIR}/native")
  target_link_libraries(khz_plugin_fs_tests PRIVATE winhttp bcrypt)
  find_package(Python3 COMPONENTS Interpreter REQUIRED)
  add_test(NAME khz_plugin_fs COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/scripts/plugin_fs_probe.py" "$<TARGET_FILE:khz_plugin_fs_tests>")
  set_tests_properties(khz_plugin_fs PROPERTIES TIMEOUT 30)
  add_executable(khz_services_tests engine/tests/khz_services_tests.cpp
    engine/src/khz/khz_content.cpp engine/src/khz/khz_content_protocol.cpp engine/src/khz/khz_browser.cpp engine/src/khz/khz_update.cpp
    engine/src/khz/khz_update_policy.cpp engine/src/khz/khz_update_bootstrap.cpp)
  target_include_directories(khz_services_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/engine/src")
  add_test(NAME khz_services COMMAND khz_services_tests)
  add_executable(khz_content_server_policy_tests tests/content_server_policy_tests.cpp)
  target_include_directories(khz_content_server_policy_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/native")
  add_test(NAME khz_content_server_policy COMMAND khz_content_server_policy_tests)
  add_executable(khz_fp_environment_tests tests/fp_environment_tests.cpp)
  target_include_directories(khz_fp_environment_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/native")
  target_compile_options(khz_fp_environment_tests PRIVATE /W4 /fp:strict /arch:SSE2)
  add_test(NAME khz_fp_environment COMMAND khz_fp_environment_tests)
  add_executable(khz_asset_paths_tests tests/asset_paths_tests.cpp
    engine/src/khz/khz_asset_paths.cpp engine/src/khz/khz_content.cpp)
  target_include_directories(khz_asset_paths_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/engine/src")
  add_test(NAME khz_asset_paths COMMAND khz_asset_paths_tests)
  add_executable(khz_stats_offsets_tests tests/stats_offsets_tests.cpp)
  target_include_directories(khz_stats_offsets_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/native")
  add_test(NAME khz_stats_offsets COMMAND khz_stats_offsets_tests)
  add_executable(khz_webadmin_policy_tests tests/webadmin_policy_tests.cpp)
  target_include_directories(khz_webadmin_policy_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/native")
  add_test(NAME khz_webadmin_policy COMMAND khz_webadmin_policy_tests)
  add_executable(khz_webadmin_service_tests tests/webadmin_service_tests.cpp
    engine/src/khz/khz_webadmin.cpp engine/src/khz/khz_identity.cpp engine/src/khz/khz_admin.cpp)
  target_include_directories(khz_webadmin_service_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/engine/src" "${CMAKE_CURRENT_SOURCE_DIR}/native")
  add_test(NAME khz_webadmin_service COMMAND khz_webadmin_service_tests)
  add_executable(khz_screenshot_tests engine/tests/khz_screenshot_tests.cpp
    engine/src/khz/khz_screenshot_protocol.cpp engine/src/khz/khz_screenshot_image.cpp engine/src/khz/khz_content.cpp)
  target_include_directories(khz_screenshot_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/engine/src")
  target_link_libraries(khz_screenshot_tests PRIVATE windowscodecs ole32 oleaut32 uuid)
  add_test(NAME khz_screenshot COMMAND khz_screenshot_tests)
endif()
if(COD4X1KHZ_BUILD_ENGINE)
  set(DXSDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/.deps/dxsdk/9.29.952.8/build/native")
  if(NOT EXISTS "${DXSDK_DIR}/include/d3dx9.h")
    message(FATAL_ERROR "Run prepare.ps1 to obtain the pinned project-local DirectX dependency")
  endif()
  set(CICD 1)
  set(TRACY_ENABLE ${COD4X1KHZ_INSTRUMENT} CACHE BOOL "" FORCE)
  set(KISAK_BUILD_SP OFF CACHE BOOL "" FORCE)
  set(KISAK_BUILD_RADIANT OFF CACHE BOOL "" FORCE)
  add_subdirectory(engine)
  foreach(pair "KisakCOD-mp;cod4x1khz" "KisakCOD-dedi;cod4x1khz_server")
    list(GET pair 0 engine_target)
    list(GET pair 1 output_name)
    target_link_libraries(${engine_target} PRIVATE khz_core dbghelp winhttp bcrypt ws2_32 iphlpapi)
    target_sources(${engine_target} PRIVATE
      "${CMAKE_CURRENT_SOURCE_DIR}/engine/src/khz/khz_runtime.cpp"
      "${CMAKE_CURRENT_SOURCE_DIR}/engine/src/khz/khz_benchmark_scene.cpp"
      "${CMAKE_CURRENT_SOURCE_DIR}/engine/src/khz/khz_snapshot_headers.cpp"
      "${CMAKE_CURRENT_SOURCE_DIR}/engine/src/khz/khz_server_metrics.cpp"
      "${CMAKE_CURRENT_SOURCE_DIR}/engine/src/khz/khz_archive.cpp"
      "${CMAKE_CURRENT_SOURCE_DIR}/engine/src/khz/khz_canonical.cpp"
      "${CMAKE_CURRENT_SOURCE_DIR}/engine/src/khz/khz_canonical_pose.cpp"
      "${CMAKE_CURRENT_SOURCE_DIR}/engine/src/khz/khz_client_capture.cpp"
      "${CMAKE_CURRENT_SOURCE_DIR}/engine/src/khz/khz_server_capture.cpp"
      "${CMAKE_CURRENT_SOURCE_DIR}/engine/src/khz/khz_client_checkpoint.cpp"
      "${CMAKE_CURRENT_SOURCE_DIR}/engine/src/khz/khz_checkpoint_probe.cpp"
      "${CMAKE_CURRENT_SOURCE_DIR}/engine/src/khz/khz_capture_probe.cpp"
      "${CMAKE_CURRENT_SOURCE_DIR}/engine/src/khz/khz_replay.cpp"
      "${CMAKE_CURRENT_SOURCE_DIR}/engine/src/khz/khz_paths.cpp"
      "${CMAKE_CURRENT_SOURCE_DIR}/engine/src/khz/khz_ui.cpp"
      "${CMAKE_CURRENT_SOURCE_DIR}/engine/src/khz/khz_host.cpp"
      "${CMAKE_CURRENT_SOURCE_DIR}/engine/src/khz/khz_bots.cpp"
      "${CMAKE_CURRENT_SOURCE_DIR}/engine/src/khz/khz_profile.cpp"
      "${CMAKE_CURRENT_SOURCE_DIR}/engine/src/khz/khz_profile_client.cpp"
      "${CMAKE_CURRENT_SOURCE_DIR}/engine/src/khz/khz_identity.cpp"
      "${CMAKE_CURRENT_SOURCE_DIR}/engine/src/khz/khz_identity_client.cpp"
      "${CMAKE_CURRENT_SOURCE_DIR}/engine/src/khz/khz_admin.cpp"
      "${CMAKE_CURRENT_SOURCE_DIR}/engine/src/khz/khz_admin_server.cpp"
      "${CMAKE_CURRENT_SOURCE_DIR}/engine/src/khz/khz_webadmin.cpp"
      "${CMAKE_CURRENT_SOURCE_DIR}/engine/src/khz/khz_plugins.cpp"
      "${CMAKE_CURRENT_SOURCE_DIR}/engine/src/khz/khz_plugin_http.cpp"
      "${CMAKE_CURRENT_SOURCE_DIR}/engine/src/khz/khz_plugin_tcp.cpp"
      "${CMAKE_CURRENT_SOURCE_DIR}/engine/src/khz/khz_plugin_net_address.cpp"
      "${CMAKE_CURRENT_SOURCE_DIR}/engine/src/khz/khz_plugin_dns.cpp"
      "${CMAKE_CURRENT_SOURCE_DIR}/engine/src/khz/khz_plugin_fs.cpp"
      "${CMAKE_CURRENT_SOURCE_DIR}/engine/src/khz/khz_plugin_fs_adapter.cpp"
      "${CMAKE_CURRENT_SOURCE_DIR}/engine/src/khz/khz_plugin_unzip.c"
      "${CMAKE_CURRENT_SOURCE_DIR}/engine/src/khz/khz_content.cpp"
      "${CMAKE_CURRENT_SOURCE_DIR}/engine/src/khz/khz_asset_paths.cpp"
      "${CMAKE_CURRENT_SOURCE_DIR}/engine/src/khz/khz_content_protocol.cpp"
      "${CMAKE_CURRENT_SOURCE_DIR}/engine/src/khz/khz_content_server.cpp"
      "${CMAKE_CURRENT_SOURCE_DIR}/engine/src/khz/khz_browser.cpp"
      "${CMAKE_CURRENT_SOURCE_DIR}/engine/src/khz/khz_services_client.cpp"
      "${CMAKE_CURRENT_SOURCE_DIR}/engine/src/khz/khz_screenshot_client.cpp"
      "${CMAKE_CURRENT_SOURCE_DIR}/engine/src/khz/khz_screenshot_server.cpp"
      "${CMAKE_CURRENT_SOURCE_DIR}/engine/src/khz/khz_screenshot_protocol.cpp"
      "${CMAKE_CURRENT_SOURCE_DIR}/engine/src/khz/khz_screenshot_image.cpp"
      "${CMAKE_CURRENT_SOURCE_DIR}/engine/src/khz/khz_update.cpp"
      "${CMAKE_CURRENT_SOURCE_DIR}/engine/src/khz/khz_update_policy.cpp"
      "${CMAKE_CURRENT_SOURCE_DIR}/engine/src/khz/khz_update_bootstrap.cpp"
      "${CMAKE_CURRENT_SOURCE_DIR}/engine/src/khz/khz_update_client.cpp"
      "${CMAKE_CURRENT_SOURCE_DIR}/engine/src/khz/khz_collision.cpp")
    target_sources(${engine_target} PRIVATE
      "${CMAKE_CURRENT_SOURCE_DIR}/engine/src/khz/khz_controls.cpp"
      "${CMAKE_CURRENT_SOURCE_DIR}/engine/src/khz/khz_client_owner.cpp"
      "${CMAKE_CURRENT_SOURCE_DIR}/engine/src/khz/khz_client_pose.cpp"
      "${CMAKE_CURRENT_SOURCE_DIR}/engine/src/khz/khz_client_projectile.cpp"
      "${CMAKE_CURRENT_SOURCE_DIR}/engine/src/khz/khz_client_body.cpp"
      "${CMAKE_CURRENT_SOURCE_DIR}/engine/src/khz/khz_client_visibility.cpp"
      "${CMAKE_CURRENT_SOURCE_DIR}/engine/src/khz/khz_client_transport.cpp"
      "${CMAKE_CURRENT_SOURCE_DIR}/engine/src/khz/khz_input_owner.cpp")
    target_compile_definitions(${engine_target} PRIVATE COD4X1KHZ=1)
    target_include_directories(${engine_target} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/sdk/include")
    target_compile_options(${engine_target} PRIVATE /fp:precise)
    set_target_properties(${engine_target} PROPERTIES OUTPUT_NAME ${output_name}
      RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/bin/$<CONFIG>")
  endforeach()
  target_sources(KisakCOD-mp PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/engine/src/khz/khz_render_profile.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/engine/src/khz/khz_frontend_upload.cpp")
  set_target_properties(KisakCOD-dedi PROPERTIES ENABLE_EXPORTS ON)
  add_library(khz_regression SHARED sdk/tests/regression_plugin.c)
  target_include_directories(khz_regression PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/sdk/include")
  target_link_libraries(khz_regression PRIVATE KisakCOD-dedi)
  set_target_properties(khz_regression PROPERTIES OUTPUT_NAME khz_regression
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_CURRENT_SOURCE_DIR}/runtime/plugins")
  add_executable(cod4x1khz_update native/update_helper.cpp
    engine/src/khz/khz_content.cpp engine/src/khz/khz_update.cpp
    engine/src/khz/khz_update_policy.cpp engine/src/khz/khz_update_bootstrap.cpp)
  target_include_directories(cod4x1khz_update PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/engine/src")
  set_target_properties(cod4x1khz_update PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_CURRENT_SOURCE_DIR}/runtime")
  add_custom_target(khz_runtime
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:KisakCOD-mp>"
      "${CMAKE_CURRENT_SOURCE_DIR}/cod4x1khz.exe"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:KisakCOD-mp>"
      "${CMAKE_CURRENT_SOURCE_DIR}/runtime/cod4x1khz.exe"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:KisakCOD-dedi>"
      "${CMAKE_CURRENT_SOURCE_DIR}/runtime/cod4x1khz_server.exe"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:KisakCOD-mp>"
      "${CMAKE_CURRENT_SOURCE_DIR}/runtime/cod4x1khz_bench.exe"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/runtime/mss32.dll" "${CMAKE_CURRENT_SOURCE_DIR}/mss32.dll"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/runtime/D3DX9_43.dll" "${CMAKE_CURRENT_SOURCE_DIR}/D3DX9_43.dll"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/runtime/binkw32.dll" "${CMAKE_CURRENT_SOURCE_DIR}/binkw32.dll"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/runtime/steam_api.dll" "${CMAKE_CURRENT_SOURCE_DIR}/steam_api.dll"
    DEPENDS KisakCOD-mp KisakCOD-dedi khz_regression cod4x1khz_update)
endif()
