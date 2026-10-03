"""Short broad-view/physical-walking renderer diagnostic with fifteen live bots.

The owned benchmark human is protected from damage and external player pushes,
and uses controlled input. This isolates camera stability, not human combat.
Bots retain ordinary combat. No intervals are removed for low FPS or camera
occlusion: a failed camera check fails the stage. This is not full-world replay.
"""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import math
from pathlib import Path
from types import SimpleNamespace
import statistics
import subprocess
import sys
import time

from native_checkpoint_capture_probe import ManagedHostFixture
from native_fov_probe import interactive_preferences, wait_recovery
from native_render_cap_probe import ManagedCapServer, admission_checks, bot_control_rates
from render_cap_probe import run_stage


def wrap(angle):
    return (angle + 180) % 360 - 180


def coordinates(points):
    return ' '.join(f'{v:.4f}' for point in points for v in point)


def geometry(f, action):
    return f.rcon('khz_scene ' + action, 'KHZ_RENDER_SCENE')


def control(f, action):
    result = f.call('khz_ui console khz_scene ' + action)
    if result.get('result') != 'queued':
        raise ValueError('Native scene input control was rejected')
    time.sleep(.1)
    return result


def open_view(status):
    rays = status.get('rays', [])
    return len(rays) == 5 and rays[2] >= 400 and statistics.median(rays) >= 300


def walking_route(f, node, heading, nodes):
    point, yaw = node['point'], heading['yaw']
    probes = []
    look = [point[0] + 1200 * math.cos(math.radians(yaw)), point[1] + 1200 * math.sin(math.radians(yaw)), point[2]]
    for index in node['links']:
        neighbor = nodes[index]['point']; length = math.dist(neighbor[:2], point[:2])
        if abs(neighbor[2] - point[2]) > 2 or length < 70:
            continue
        fraction = min(1, 100 / length)
        candidate = [point[i] + (neighbor[i] - point[i]) * fraction for i in range(3)]
        segment = geometry(f, 'segment ' + coordinates([point, candidate])); entry = {'endpoint': candidate, 'segment': segment}; probes.append(entry)
        if not segment.get('clear'):
            continue
        good = True
        for t in (.5, 1):
            pos = [point[i] + (candidate[i] - point[i]) * t for i in range(3)]
            value = geometry(f, 'probe ' + coordinates([pos])); entry.setdefault('views', []).append(value)
            aimed = math.degrees(math.atan2(look[1] - pos[1], look[0] - pos[0]))
            closest = min(value.get('headings', []), key=lambda h: abs(wrap(h['yaw'] - aimed)))
            good &= value.get('clear') is True and open_view(closest)
        if good:
            return {'endpoint': candidate, 'look': look, 'probes': probes}
    return {'endpoint': None, 'probes': probes}


def choose(f, root, map_name, require_walk=False):
    path = root / 'upstream/bot_warfare/scriptdata/waypoints' / (map_name + '_wp.csv')
    lines = path.read_text().splitlines()
    nodes = []
    for index, line in enumerate(lines[1:]):
        columns = line.split(',')
        nodes.append({'index': index, 'point': list(map(float, columns[0].split())),
                      'links': list(map(int, columns[1].split())), 'stance': columns[2]})
    center = [statistics.mean(n['point'][i] for n in nodes) for i in range(3)]
    # Well-connected walking nodes around the field center. All choices are
    # then checked against the actual loaded native collision map.
    candidates = sorted((n for n in nodes if n['stance'] == 'stand' and len(n['links']) >= 4),
        key=lambda n: (n['point'][0] - center[0]) ** 2 + (n['point'][1] - center[1]) ** 2)[:8]
    probes = []
    choices = []
    for n in candidates:
        value = geometry(f, 'probe ' + coordinates([n['point']]))
        probes.append({'node': n, 'native': value})
        target_yaw = math.degrees(math.atan2(center[1] - n['point'][1], center[0] - n['point'][0]))
        for heading in value.get('headings', []) if value.get('clear') else []:
            if abs(wrap(heading['yaw'] - target_yaw)) <= 45 and open_view(heading):
                choices.append((statistics.median(heading['rays']), n, heading))
    if not choices:
        raise ValueError('No collision-checked broad view found; never substitute a wall-facing view')
    # Choose breadth among views facing the playable area, rather than the
    # fastest measured view. FPS has not been measured at selection time.
    evidence = {'csv': str(path), 'csv_sha256': hashlib.sha256(path.read_bytes()).hexdigest(),
                'center': center, 'probes': probes, 'rejected_routes': []}
    for _, node, heading in sorted(choices, key=lambda item: item[0], reverse=True):
        if require_walk:
            route = walking_route(f, node, heading, nodes)
            if route['endpoint'] is None:
                evidence['rejected_routes'].append({'node': node['index'], 'yaw': heading['yaw'], 'route': route})
                continue
            evidence['walking_route'] = route
        evidence.update(selected=node, heading=heading)
        return node, heading, nodes, evidence
    raise ValueError('No clear connected walking segment with a broad view; no fallback wall scene')


def place(f, point, yaw):
    action = f'place {f.port} ' + coordinates([point]) + f' {yaw:.4f} 0'
    for attempt in range(6):
        value = geometry(f, action)
        if value.get('placed'):
            return value
        if value.get('error') != 'active human not found':
            break
        time.sleep(.5)
    raise ValueError('Native camera placement failed: ' + json.dumps(value))


def wait_view(f, point, yaw):
    until = time.monotonic() + 6
    while time.monotonic() < until:
        sample = f.sample()
        state = sample['state']
        view = state.get('rendered_view_angles', [])
        if (state.get('pm_type') == 0 and len(view) == 3 and abs(wrap(view[1] - yaw)) < 2 and abs(wrap(view[0])) < 2 and
            math.dist(state.get('origin', [1e9] * 3)[:2], point[:2]) < 20):
            return sample
        time.sleep(.1)
    raise ValueError('Actual rendered camera did not settle at the selected broad view')


def fps_distribution(stage):
    intervals = []
    samples = stage['samples']
    for a, b in zip(samples, samples[1:]):
        a, b = a['stats'], b['stats']
        seconds = b['seconds'] - a['seconds']
        if seconds <= 0 or b['gpu_presents'] < a['gpu_presents']:
            raise ValueError('Invalid actual presentation sample interval')
        intervals.append((b['gpu_presents'] - a['gpu_presents']) / seconds)
    values = sorted(intervals)
    return {'sample_interval_seconds': 'approximately one second; these are not individual-frame 1% lows',
            'intervals_fps': intervals, 'minimum_fps': min(values), 'p10_fps': values[math.ceil(.1 * len(values)) - 1],
            'median_fps': statistics.median(values), 'maximum_fps': max(values)}


def check_camera(stage, point, yaw=None, end=None, look=None):
    failures = []
    positions = []
    for index, sample in enumerate(stage['samples']):
        state = sample['state']; origin = state.get('origin', []); view = state.get('rendered_view_angles', [])
        if len(origin) != 3 or len(view) != 3 or state.get('pm_type') != 0:
            failures.append(f'Sample {index} lacks a live actual view'); continue
        positions.append(origin)
        if end is None:
            if math.dist(origin[:2], point[:2]) > 20 or abs(wrap(view[1] - yaw)) > 2 or abs(wrap(view[0])) > 2:
                failures.append(f'Sample {index} left its fixed view')
        else:
            if look is None or abs(wrap(view[0])) > 2 or abs(wrap(view[1] - math.degrees(math.atan2(look[1] - origin[1], look[0] - origin[0])))) > 2:
                failures.append(f'Sample {index} stopped facing the checked open view')
            delta = [end[i] - point[i] for i in range(2)]; norm = sum(v * v for v in delta)
            t = sum((origin[i] - point[i]) * delta[i] for i in range(2)) / norm
            projected = [point[i] + max(0, min(1, t)) * delta[i] for i in range(2)]
            if math.dist(origin[:2], projected) > 30:
                failures.append(f'Sample {index} left its checked walking segment')
    if end is not None and (not positions or max(math.dist(a[:2], b[:2]) for a in positions for b in positions) < 30):
        failures.append('The native player did not actually walk at least thirty units')
    return {'passed': not failures, 'errors': failures, 'positions': positions}


def screenshot(f, output):
    before = set(f.home.rglob('shot*.jpg')) | set(f.home.rglob('shot*.tga'))
    value = f.call('khz_ui screenshot')
    if value.get('result') != 'queued':
        raise ValueError('Actual native screenshot request was rejected')
    until = time.monotonic() + 4
    while time.monotonic() < until:
        new = (set(f.home.rglob('shot*.jpg')) | set(f.home.rglob('shot*.tga'))) - before
        if new:
            source = sorted(new)[0]
            data = source.read_bytes()
            if len(data) > 1000:
                from native_screenshot_png import convert
                target = output / ('broad-view.png' if source.suffix == '.tga' else 'broad-view.jpg')
                png = convert(data) if source.suffix == '.tga' else data; target.write_bytes(png)
                return {'path': str(target), 'sha256': hashlib.sha256(png).hexdigest(), 'native_source_sha256': hashlib.sha256(data).hexdigest()}
        time.sleep(.1)
    raise ValueError('Native broad-view screenshot was not saved in the isolated home')


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--build', type=int, default=86)
    p.add_argument('--map', choices=('mp_killhouse', 'mp_crash'), default='mp_killhouse')
    p.add_argument('--resolution', choices=('480p', '720p'), default='480p')
    p.add_argument('--client-port', type=int, default=28995)
    p.add_argument('--timing-seconds', type=int, choices=(0, 60), default=0,
                   help='60 runs one strict 0.1%% uncapped walking timing gate; default runs the short cap stages')
    a = p.parse_args(); root = Path(__file__).resolve().parent.parent
    archive = root / f'results/build{a.build}-source-and-binary-hashes.json'
    record = json.loads(archive.read_text(encoding='utf-8-sig'))
    names = ('cod4x1khz.exe', 'runtime/cod4x1khz_server.exe')
    if any(hashlib.sha256((root / n).read_bytes()).hexdigest() != record['binary_sha256'][n] for n in names):
        raise ValueError('Native binaries differ from selected exact build')
    out = root / 'results' / (f'native-render-scene{a.build}-{a.map}-{a.resolution}-' + datetime.now(timezone.utc).strftime('%Y%m%d-%H%M%S-%f'))
    out.mkdir(); f = ManagedHostFixture(root, out, a.client_port, budget=180, resolution=a.resolution)
    prefs = interactive_preferences(root); observer = None
    f.report.update(map=a.map, resolution=a.resolution, build_archive_sha256=hashlib.sha256(archive.read_bytes()).hexdigest(),
        scene_probe_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
        render_cap_probe_sha256=hashlib.sha256((root / 'scripts/render_cap_probe.py').read_bytes()).hexdigest(),
        timing_seconds=a.timing_seconds, rate_tolerance_hz=1 if a.timing_seconds else 2,
        scope=__doc__, stages=[], full_world_replay_verified=False)
    try:
        f.start()
        if a.map != 'mp_killhouse':
            before = f.sample(); f.call('khz_ui server map ' + a.map)
            wait_recovery(f, 'broad-view map', a.map, before, changed_map=True)
        node, heading, nodes, evidence = choose(f, root, a.map, require_walk=True)
        f.report['scene_selection'] = evidence
        point, yaw = node['point'], heading['yaw']
        control(f, f'fixed {yaw} 0'); f.report['placement'] = place(f, point, yaw)
        f.report['settled'] = wait_view(f, point, yaw)
        f.report['scene_before'] = geometry(f, f'status {f.port}')
        if not open_view(f.report['scene_before']) or not f.report['scene_before'].get('external_push_protected'):
            raise ValueError('Actual native human camera still faces nearby occlusion')
        f.report['screenshot'] = screenshot(f, out)
        # Selection and capsule/view checks precede every FPS measurement.
        route = evidence['walking_route']; endpoint, look = route['endpoint'], route['look']
        f.report['route_selection'] = route['probes']
        observer = subprocess.Popen([sys.executable, str(root / 'scripts/render_usage_observer.py'), '--client', str(f.client.pid),
            '--server', str(f.server.pid), '--seconds', '65', '--output', str(out / 'usage.json')], creationflags=subprocess.CREATE_NO_WINDOW)
        args = SimpleNamespace(client_port=f.port, map=a.map, resolution=a.resolution, desired_bots=15,
                               measure_seconds=a.timing_seconds or 8, rate_tolerance_hz=1 if a.timing_seconds else 2)
        server = ManagedCapServer(f)
        if a.timing_seconds:
            control(f, 'shuttle ' + coordinates([point, endpoint, look]))
        for cap in (0,) if a.timing_seconds else (125, 240, 1000, 0):
            stage = run_stage(cap, args, server); stage['scene'] = 'physical walking segment' if a.timing_seconds else 'fixed broad view'
            stage['camera'] = check_camera(stage, point, end=endpoint, look=look) if a.timing_seconds else check_camera(stage, point, yaw)
            stage['fps_intervals'] = fps_distribution(stage)
            stage['admission'] = admission_checks(stage, f, a.resolution); stage['bot_controls'] = bot_control_rates(stage)
            if a.timing_seconds:
                stage['strict_0_1_percent_pass'] = abs(stage['admission']['accepted_ack_input_hz'] - 1000) <= 1 and all(
                    slot['command_ticks_hz'] is not None and abs(slot['command_ticks_hz'] - 1000) <= 1 for slot in stage['bot_controls']['slots'].values())
                stage['passed'] &= stage['strict_0_1_percent_pass']
            stage['passed'] &= stage['camera']['passed'] and stage['admission']['passed'] and stage['bot_controls']['passed']
            f.report['stages'].append(stage)
            print(f"Broad view cap {cap or 'uncapped'}: {stage['client']['gpu_presents_hz']:.1f} FPS; lowest second {stage['fps_intervals']['minimum_fps']:.1f}; passed={stage['passed']}", flush=True)
        f.report['scene_after'] = geometry(f, f'status {f.port}')
        if not open_view(f.report['scene_after']) or not f.report['scene_after'].get('external_push_protected'):
            raise ValueError('Actual broad view became occluded; all measured intervals remain retained')
        if not a.timing_seconds:
            control(f, 'shuttle ' + coordinates([point, endpoint, look]))
            stage = run_stage(0, args, server); stage['scene'] = 'physical walking segment'
            stage['camera'] = check_camera(stage, point, end=endpoint, look=look); stage['fps_intervals'] = fps_distribution(stage)
            stage['admission'] = admission_checks(stage, f, a.resolution); stage['bot_controls'] = bot_control_rates(stage)
            stage['passed'] &= stage['camera']['passed'] and stage['admission']['passed'] and stage['bot_controls']['passed']
            f.report['stages'].append(stage)
            print(f"Walking: {stage['client']['gpu_presents_hz']:.1f} FPS; lowest second {stage['fps_intervals']['minimum_fps']:.1f}; passed={stage['passed']}", flush=True)
        f.report['walk_after'] = geometry(f, f'status {f.port}')
        if not open_view(f.report['walk_after']):
            raise ValueError('Actual walking camera ended facing nearby occlusion')
        observer.wait(timeout=15)
        f.report['passed'] = len(f.report['stages']) == (1 if a.timing_seconds else 5) and all(s['passed'] for s in f.report['stages'])
        if not f.report['passed']: f.report['errors'].append('A native view, input, bot, rate, work or cap check failed')
        f.report['protection_release'] = geometry(f, f'release {f.port}')
        if not f.report['protection_release'].get('released') or f.report['protection_release'].get('god') != f.report['placement']['previous_god']:
            raise ValueError('Diagnostic protection did not restore the original human damage setting')
        control(f, 'legacy'); f.leave()
    except Exception as e:
        f.report['passed'] = False; f.report['errors'].append(str(e))
    finally:
        f.close()
        if observer and observer.poll() is None:
            observer.terminate(); observer.wait(timeout=5)
        f.report['interactive_preferences_unchanged'] = prefs == interactive_preferences(root)
        f.report['binary_provenance_pass'] = all(hashlib.sha256((root / n).read_bytes()).hexdigest() == record['binary_sha256'][n] for n in names)
        f.report['passed'] &= not f.report['errors'] and f.report['interactive_preferences_unchanged'] and f.report['binary_provenance_pass'] and f.report['cleanup']['created_client_returncode'] == 0 and f.report['cleanup']['exact_owned_server_exited']
        (out / 'result.json').write_text(json.dumps(f.report, indent=2) + '\n')
    print(json.dumps({'passed': f.report['passed'], 'errors': f.report['errors'], 'output': str(out)}, indent=2))
    return 0 if f.report['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
