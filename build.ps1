param([ValidateSet('Release','Debug')][string]$Configuration = 'Release',
    [ValidateSet('Win32','x64')][string]$Architecture = 'Win32',
    [switch]$CoreOnly, [switch]$Instrument)
$ErrorActionPreference = 'Stop'
# Native evidence tests write here; a fresh Git clone has no ignored results folder.
New-Item -ItemType Directory -Force -Path (Join-Path $PSScriptRoot 'results') | Out-Null
$buildDirectory = if ($Architecture -eq 'x64') { "build/x64/$Configuration" } else { "build/$Configuration" }
$buildRoot = Join-Path $PSScriptRoot $buildDirectory
$buildOptions = @('-S', $PSScriptRoot, '-B', $buildRoot, '-G', 'Visual Studio 17 2022', '-A', $Architecture, "-DCMAKE_BUILD_TYPE=$Configuration")
if ($CoreOnly) { $buildOptions += @('-DCOD4X1KHZ_BUILD_ENGINE=OFF','-DCOD4X1KHZ_BUILD_VULKAN=OFF') }
else { $buildOptions += @('-DCOD4X1KHZ_BUILD_ENGINE=ON','-DCOD4X1KHZ_BUILD_VULKAN=ON') }
if ($Instrument) { $buildOptions += '-DCOD4X1KHZ_INSTRUMENT=ON' }
else { $buildOptions += '-DCOD4X1KHZ_INSTRUMENT=OFF' }
if (!$CoreOnly -and $Architecture -eq 'x64') {
    $nativeVideoPackage = & (Join-Path $PSScriptRoot 'scripts/prepare-native-video.ps1')
    $buildOptions += "-DKHZ_FFMPEG_ROOT=$nativeVideoPackage"
    $buildOptions += '-DCOD4X1KHZ_CLIENT_IPO=ON'
}
& cmake @buildOptions
if ($LASTEXITCODE) { throw 'Configure failed' }
if (!$CoreOnly) { & (Join-Path $PSScriptRoot 'scripts/package-bots.ps1') }
$targets = @('khz_tests', 'khz_publication_tests', 'khz_snapshot_header_history_tests', 'khz_identity_admin_tests', 'khz_input_owner_tests', 'khz_plugin_registry_tests', 'khz_plugin_io_policy_tests', 'khz_plugin_http_tests', 'khz_services_tests', 'khz_content_server_policy_tests', 'khz_fp_environment_tests', 'khz_asset_paths_tests', 'khz_stats_offsets_tests', 'khz_webadmin_policy_tests', 'khz_webadmin_service_tests', 'khz_screenshot_tests')
$targets += @('khz_canonical_tests', 'khz_replay_capture_tests', 'khz_gameplay_rng_tests', 'khz_service_control_policy_tests', 'khz_input_canonical_tests', 'khz_command_arguments_tests', 'khz_snapshot_reader_window_tests')
$targets += @('khz_client_checkpoint_tests', 'khz_input_replay_policy_tests', 'khz_plugin_tcp_tests', 'khz_plugin_fs_tests')
$targets += 'khz_connectionless_policy_tests'
$targets += 'khz_server_capture_tests'
$targets += @('khz_server_checkpoint_tests', 'khz_plugin_net_address_tests')
$targets += @('khz_input_timestamp_tests', 'khz_plugin_packet_mailbox_tests')
$targets += 'khz_plugin_dns_tests'
$targets += 'khz_host_checkpoint_policy_tests'
$targets += 'khz_huffman_native_tests'
$targets += @('khz_msg_bits_native_tests', 'khz_park_wait_tests', 'khz_steam_identity_format_tests')
$targets += 'khz_archive_history_tests'
$targets += 'khz_debug_context_tests'
$targets += 'khz_pool_allocator_native_tests'
$targets += 'khz_asset_wire_tests'
$targets += 'khz_asset_stream_native_tests'
$targets += @('khz_model_skin_native_tests','khz_movement_friction_native_tests','khz_fx_mark_mesh_native_tests')
$targets += 'khz_entity_pose_native_tests'
$targets += 'khz_ragdoll_reset_native_tests'
$targets += 'khz_frontend_upload_native_tests'
$targets += 'khz_ui_scroll_native_tests'
$targets += 'khz_hunk_memory_native_tests'
$targets += 'khz_plugin_sdk_native_tests'
$targets += 'khz_script_value_native_tests'
$targets += 'khz_dvar_values_native_tests'
$targets += @('khz_player_use_native_tests', 'khz_filesystem_native_tests', 'khz_scoreboard_native_tests', 'khz_fx_archive_native_tests')
$targets += 'khz_collision_lifetime_native_tests'
$targets += @('khz_ui_values_native_tests','khz_render_commands_native_tests')
$targets += 'khz_dpvs_entity_native_tests'
$targets += 'khz_hud_boolean_native_tests'
$targets += @('khz_fx_model_physics_native_tests','khz_mouse_capture_tests')
$targets += @('khz_mark_context_native_tests','khz_pcm_prefetch_tests')
$targets += 'khz_server_profile_tests'
if ($Architecture -eq 'x64' -and (Test-Path (Join-Path $buildRoot 'CMakeCache.txt')) -and
    (Select-String -LiteralPath (Join-Path $buildRoot 'CMakeCache.txt') -Pattern '^KHZ_INSTALLER_PAYLOAD:FILEPATH=.+')) {
    $targets += 'khz_install_package_tests'
}
if (!$CoreOnly -and $Architecture -eq 'x64') { $targets += @('cod4x1khz_server_manager','cod4x1khz_network','khz_sound_openal_native_tests') }
if (!$CoreOnly) { $targets += @('khz_runtime','khz_vulkan_probe','khz_vulkan_shader_inventory','khz_vulkan_shader_link','khz_vulkan_shader_tests','khz_vulkan_backend') }
& cmake --build $buildRoot --config $Configuration --target @targets --parallel 8
if ($LASTEXITCODE) { throw 'Build failed' }
& ctest --test-dir $buildRoot -C $Configuration --output-on-failure
if ($LASTEXITCODE) { throw 'Tests failed' }
