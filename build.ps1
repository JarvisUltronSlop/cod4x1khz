param([ValidateSet('Release','Debug')][string]$Configuration = 'Release', [switch]$CoreOnly, [switch]$Instrument)
$ErrorActionPreference = 'Stop'
$buildRoot = Join-Path $PSScriptRoot "build/$Configuration"
$buildOptions = @('-S', $PSScriptRoot, '-B', $buildRoot, '-G', 'Visual Studio 17 2022', '-A', 'Win32', "-DCMAKE_BUILD_TYPE=$Configuration")
if ($CoreOnly) { $buildOptions += '-DCOD4X1KHZ_BUILD_ENGINE=OFF' }
else { $buildOptions += '-DCOD4X1KHZ_BUILD_ENGINE=ON' }
if ($Instrument) { $buildOptions += '-DCOD4X1KHZ_INSTRUMENT=ON' }
else { $buildOptions += '-DCOD4X1KHZ_INSTRUMENT=OFF' }
if (!$CoreOnly) { & (Join-Path $PSScriptRoot 'scripts/package-bots.ps1') }
& cmake @buildOptions
if ($LASTEXITCODE) { throw 'Configure failed' }
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
if (!$CoreOnly) { $targets += 'khz_runtime' }
& cmake --build $buildRoot --config $Configuration --target @targets --parallel 8
if ($LASTEXITCODE) { throw 'Build failed' }
& ctest --test-dir $buildRoot -C $Configuration --output-on-failure
if ($LASTEXITCODE) { throw 'Tests failed' }
