"""Owned, isolated Vulkan loaded-world diagnostic; no FPS/replay acceptance claim."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import shutil
import time

from native_checkpoint_capture_probe import ManagedHostFixture
from native_fov_probe import interactive_preferences, wait_recovery
from native_render_scene_probe import choose, place, wait_view, control, check_camera
from native_screenshot_png import convert, convert_bgra


def capture_world(fixture, output):
    report = fixture.report
    report['capture_before'] = fixture.sample()
    report['capture_request'] = fixture.call('khz_ui console screenshot vulkan-world')
    if report['capture_request'].get('result') != 'queued':
        raise RuntimeError('Lossless native world screenshot was rejected')
    deadline = time.monotonic()+4
    while time.monotonic()<deadline:
        captures = list(fixture.home.rglob('vulkan-world.tga'))
        if captures:
            data = captures[0].read_bytes()
            if len(data)>1000:
                (output/'world.tga').write_bytes(data)
                (output/'world.png').write_bytes(convert(data))
                report['capture'] = {'path': str(output/'world.png'), 'native_sha256': hashlib.sha256(data).hexdigest()}
                reference = fixture.home/'vulkan/diagnostics/reference.json'
                if reference.is_file():
                    metadata = json.loads(reference.read_text())
                    report['packet_comparison'] = metadata
                    shutil.copy2(reference, output/'reference.json')
                    for api in ('d3d9', 'vulkan'):
                        raw = reference.parent/f'reference-{api}.bgra'
                        pixels = raw.read_bytes()
                        if len(pixels)!=metadata['width']*metadata['height']*4:
                            raise RuntimeError('Immutable-packet capture is not an exact BGRA surface')
                        shutil.copy2(raw, output/raw.name)
                        (output/f'reference-{api}.png').write_bytes(convert_bgra(metadata['width'],metadata['height'],pixels))
                report['capture_after'] = fixture.sample()
                return
        time.sleep(.1)
    raise RuntimeError('Lossless world screenshot was not saved')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', type=int, default=28997)
    parser.add_argument('--map', default='mp_killhouse', help='Installed stock multiplayer map')
    parser.add_argument('--resolution', choices=('480p', '720p'), default='480p')
    parser.add_argument('--api', choices=('vulkan', 'd3d9'), default='vulkan')
    parser.add_argument('--compare', action='store_true', help='Mirror identical immutable resources/draws into a private DX9 reference; diagnostic overhead invalidates FPS acceptance')
    parser.add_argument('--validation', action=argparse.BooleanOptionalAction, default=True)
    parser.add_argument('--state-cache', action=argparse.BooleanOptionalAction, default=True)
    parser.add_argument('--capture', action='store_true', help='Place a checked broad-view camera and retain a lossless world screenshot')
    parser.add_argument('--samples', type=int, choices=range(2, 16), default=6)
    parser.add_argument('--cap', type=int, choices=(0, 125, 240, 1000), default=125)
    args = parser.parse_args()
    if args.compare and args.api != 'vulkan':
        parser.error('--compare requires the Vulkan client')
    root = Path(__file__).resolve().parent.parent
    maps = {p.stem for p in (root/'runtime/zone/english').glob('mp_*.ff') if not p.stem.endswith('_load')}
    if args.map not in maps:
        parser.error('--map must name an installed stock multiplayer zone')
    output = root/'results'/('vulkan-world-'+datetime.now(timezone.utc).strftime('%Y%m%d-%H%M%S'))
    output.mkdir()
    runtime = root/'runtime/cod4x1khz.exe'
    source = root/'bin/Release/cod4x1khz.exe'
    saved = output/'preserved-runtime-client.exe'
    shutil.copy2(runtime, saved)
    tested_hash = hashlib.sha256(source.read_bytes()).hexdigest()
    preferences = interactive_preferences(root)
    saved_layer = os.environ.get('VK_LAYER_PATH')
    fixture = None
    report = {'scope': __doc__, 'passed': False, 'errors': [], 'tested_sha256': tested_hash,
              'full_world_replay_verified': False, 'timing_acceptance_verified': False}
    try:
        shutil.copy2(source, runtime)
        os.environ['VK_LAYER_PATH'] = str(root/'.deps/vulkan/layers/Release')
        fixture = ManagedHostFixture(root, output, args.port, budget=90, resolution=args.resolution,
            client_binary=runtime, client_settings={'r_graphicsApi': args.api, 'r_vkValidation': int(args.validation), 'r_vkCompare': int(args.compare), 'r_vkStateCache': int(args.state_cache), 'com_maxfps': args.cap})
        report = fixture.report
        report.update(scope=__doc__, api=args.api, map=args.map, state_cache_enabled=args.state_cache, validation_requested=args.validation, comparison_requested=args.compare, full_world_replay_verified=False, timing_acceptance_verified=False)
        fixture.start()
        if args.map != 'mp_killhouse':
            before = fixture.sample()
            report['map_request'] = fixture.call('khz_ui server map '+args.map)
            wait_recovery(fixture, 'Vulkan stock map', args.map, before, changed_map=True)
        camera = None
        if args.capture:
            node, heading, _, evidence = choose(fixture, root, args.map)
            report['camera_selection'] = evidence
            control(fixture, f"fixed {heading['yaw']} 0")
            report['camera_placement'] = place(fixture, node['point'], heading['yaw'])
            wait_view(fixture, node['point'], heading['yaw'])
            camera = (node['point'], heading['yaw'])
        report['world_samples'] = [fixture.sample()]
        for _ in range(args.samples):
            time.sleep(1)
            report['world_samples'].append(fixture.sample())
        first, last = report['world_samples'][0]['stats'], report['world_samples'][-1]['stats']
        seconds = last['seconds']-first['seconds']
        report['diagnostic_rates'] = {name: (last[name]-first[name])/seconds for name in
            ('gpu_presents', 'prediction_ticks', 'commands_sent', 'snapshots_received')}
        if camera:
            report['camera_check'] = check_camera({'samples': report['world_samples']}, *camera)
            if not report['camera_check']['passed']:
                raise RuntimeError('Actual rendered camera left the checked broad view')
            capture_world(fixture, output)
        native = [s['stats'].get('native_graphics', {}) for s in report['world_samples']]
        if 'capture_after' in report:
            native.append(report['capture_after']['stats'].get('native_graphics', {}))
        report['validation_errors'] = max(s.get('validation_errors', 0) for s in native)
        if args.api == 'vulkan' and not all(s.get('active') for s in native):
            raise RuntimeError('Requested Vulkan backend was not active during the world diagnostic')
        if args.validation and report['validation_errors']:
            raise RuntimeError('Loaded-world Vulkan validation errors were reported')
        report['passed'] = True
    except Exception as error:
        report['errors'].append(type(error).__name__+': '+str(error))
    finally:
        if fixture:
            fixture.close()
        if saved_layer is None:
            os.environ.pop('VK_LAYER_PATH', None)
        else:
            os.environ['VK_LAYER_PATH'] = saved_layer
        if hashlib.sha256(runtime.read_bytes()).hexdigest() == tested_hash:
            shutil.copy2(saved, runtime)
        report['runtime_restored'] = hashlib.sha256(runtime.read_bytes()).digest() == hashlib.sha256(saved.read_bytes()).digest()
        report['interactive_preferences_unchanged'] = interactive_preferences(root) == preferences
        report['passed'] &= report['runtime_restored'] and report['interactive_preferences_unchanged']
        (output/'result.json').write_text(json.dumps(report, indent=2)+'\n')
        print(json.dumps({'output': str(output), 'passed': report['passed'], 'errors': report['errors'], 'diagnostic_rates': report.get('diagnostic_rates')}), flush=True)
    raise SystemExit(0 if report['passed'] else 1)


if __name__ == '__main__':
    main()
