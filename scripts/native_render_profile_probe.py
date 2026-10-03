"""Actual 8-second renderer capture on an exact owned native client/server pair.

This diagnostic adds GPU queries and is not a sustained performance acceptance run.
"""
from pathlib import Path
from datetime import datetime, timezone
import argparse
import hashlib
import json
import time
from native_checkpoint_capture_probe import ManagedHostFixture
from native_fov_probe import interactive_preferences

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--client-port', type=int, default=28993)
    parser.add_argument('--resolution', choices=('480p', '720p'), default='480p')
    parser.add_argument('--build', type=int, default=86)
    parser.add_argument('--broad-view', action='store_true')
    parser.add_argument('--upload-mode', type=int, choices=(0, 1), default=1)
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    archive = root / f'results/build{args.build}-source-and-binary-hashes.json'
    record = json.loads(archive.read_text(encoding='utf-8-sig'))
    for name in ('cod4x1khz.exe', 'runtime/cod4x1khz_server.exe'):
        if hashlib.sha256((root / name).read_bytes()).hexdigest() != record['binary_sha256'][name]:
            raise ValueError('Native binaries differ from selected build archive')
    output = root / 'results' / (f'native-render-profile{args.build}-' + datetime.now(timezone.utc).strftime('%Y%m%d-%H%M%S-%f'))
    output.mkdir()
    fixture = ManagedHostFixture(root, output, args.client_port, resolution=args.resolution)
    fixture.report['build_archive_sha256'] = hashlib.sha256(archive.read_bytes()).hexdigest()
    fixture.report['profiling_only'] = True
    preferences = interactive_preferences(root)
    try:
        fixture.start()
        if args.broad_view:
            from native_render_scene_probe import choose, control, place, wait_view, geometry, open_view, screenshot
            node, heading, _, scene = choose(fixture, root, 'mp_killhouse')
            fixture.report['scene_selection'] = scene
            control(fixture, f"fixed {heading['yaw']} 0")
            fixture.report['scene_placement'] = place(fixture, node['point'], heading['yaw'])
            fixture.report['scene_settled'] = wait_view(fixture, node['point'], heading['yaw'])
            fixture.report['scene_before'] = geometry(fixture, f'status {fixture.port}')
            if not open_view(fixture.report['scene_before']):
                raise ValueError('Actual profile camera is near occlusion')
            fixture.report['screenshot'] = screenshot(fixture, output)
        fixture.report['upload_mode'] = fixture.call('khz_ui renderprofile uploadmode ' + str(args.upload_mode))
        time.sleep(1)
        fixture.report['before'] = fixture.sample()
        fixture.report['uploads_before'] = fixture.call('khz_ui renderprofile uploads')
        fixture.report['request'] = fixture.call('khz_ui renderprofile start 8000')
        statuses = fixture.report['profile_statuses'] = []
        until = time.monotonic() + 14
        while time.monotonic() < until:
            status = fixture.call('khz_ui renderprofile status')
            statuses.append(status)
            if status.get('phase') == 4:
                break
            if status.get('phase') == 5:
                raise ValueError('Native capture aborted: ' + json.dumps(status))
            time.sleep(.4)
        else:
            raise TimeoutError('Native profile did not finish within fourteen seconds')
        if status.get('cpu_overflow') or status.get('query_errors') or status.get('gpu_skipped'):
            raise ValueError('Bounded native profile lost records or query operations failed')
        fixture.report['export'] = fixture.call('khz_ui renderprofile export', timeout=4)
        path = fixture.home / ('render-profile-' + str(status['start_qpc']) + '.json')
        data = json.loads(path.read_text())
        if data['status'].get('phase') != 4 or len(data['gpu']) != status['gpu_frames'] or len(data['cpu']) != status['cpu_main'] + status['cpu_backend']:
            raise ValueError('Native capture file differs from completed frozen counters')
        if status.get('gpu_supported') and (status.get('gpu_frequency', 0) <= 0 or status.get('disjoint') or status['gpu_ready'] != status['gpu_frames']):
            raise ValueError('Native GPU timestamps incomplete or disjoint')
        fixture.report['capture_file'] = {'relative_to_home': path.name, 'sha256': hashlib.sha256(path.read_bytes()).hexdigest(), 'bytes': path.stat().st_size}
        fixture.report['after'] = fixture.sample()
        fixture.report['uploads_after'] = fixture.call('khz_ui renderprofile uploads')
        if args.broad_view:
            fixture.report['scene_after'] = geometry(fixture, f'status {fixture.port}')
            if not open_view(fixture.report['scene_after']):
                raise ValueError('Actual profile camera became occluded')
            from native_render_scene_probe import check_camera
            fixture.report['camera'] = check_camera({'samples': [fixture.report['before'], fixture.report['after']]}, node['point'], heading['yaw'])
            if not fixture.report['camera']['passed']:
                raise ValueError('Native profile camera left its selected view')
        a, b = fixture.report['uploads_before'], fixture.report['uploads_after']
        if a['enabled'] != bool(args.upload_mode) or b['enabled'] != bool(args.upload_mode) or b['failures']:
            raise ValueError('Actual frontend upload mode or ownership failed')
        if args.upload_mode and (b['upload_frames'] <= a['upload_frames'] or b['staged_locks'] <= a['staged_locks']):
            raise ValueError('No actual staged geometry uploads were observed')
        if not args.upload_mode and any(b[k] != a[k] for k in ('upload_frames', 'staged_locks', 'upload_bytes')):
            raise ValueError('Legacy comparison unexpectedly used staged geometry')
        fixture.report['passed'] = True
        if args.broad_view:
            fixture.report['protection_release'] = geometry(fixture, f'release {fixture.port}')
            if not fixture.report['protection_release'].get('released') or fixture.report['protection_release'].get('god') != fixture.report['scene_placement']['previous_god']:
                raise ValueError('Diagnostic camera protection did not restore the original damage setting')
            control(fixture, 'legacy')
        fixture.leave()
    except Exception as error:
        fixture.report['passed'] = False
        fixture.report['errors'].append(str(error))
    finally:
        fixture.close()
        fixture.report['interactive_preferences_unchanged'] = preferences == interactive_preferences(root)
        fixture.report['binary_provenance_pass'] = all(hashlib.sha256((root / name).read_bytes()).hexdigest() == record['binary_sha256'][name]
            for name in ('cod4x1khz.exe', 'runtime/cod4x1khz_server.exe'))
        fixture.report['passed'] &= fixture.report['interactive_preferences_unchanged'] and fixture.report['binary_provenance_pass']
        fixture.report['passed'] &= not fixture.report['errors'] and fixture.report['cleanup']['created_client_returncode'] == 0 and fixture.report['cleanup']['exact_owned_server_exited']
        (output / 'result.json').write_text(json.dumps(fixture.report, indent=2) + '\n')
    print(json.dumps({'passed': fixture.report['passed'], 'errors': fixture.report['errors'], 'output': str(output)}, indent=2))
    return 0 if fixture.report['passed'] else 1

if __name__ == '__main__':
    raise SystemExit(main())
