"""Change presentation caps on one live client while checking fixed 1 kHz work."""
import argparse
import hashlib
import json
from pathlib import Path
import socket
import time
from datetime import datetime, timezone
from monitor import collision_path, independent_owner_path, query


CAPS = (125, 240, 1000, 0)
WARM_SECONDS = 2
MEASURE_SECONDS = 8
RATE_TOLERANCE_HZ = 2


class ServerQueries:
    """Respect the engine's one RCON request/second/address restriction."""
    def __init__(self, port):
        self.port = port
        self.last_request = float('-inf')

    def _pace(self):
        delay = self.last_request + 1.1 - time.monotonic()
        if delay > 0:
            time.sleep(delay)
        self.last_request = time.monotonic()

    def reset_work(self):
        self._pace()
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
            sock.sendto(b'\xff' * 4 + b'rcon local-khz-benchmark khz_resetstats',
                        ('127.0.0.1', self.port))

    def sample(self):
        self._pace()
        return query(self.port, 'rcon local-khz-benchmark khz_json')


def rates(first, last, names):
    duration = last['seconds'] - first['seconds']
    if duration <= 0:
        raise RuntimeError('Telemetry clock did not advance')
    result = {}
    for name in names:
        delta = last[name] - first[name]
        if delta < 0:
            raise RuntimeError(f'{name} counter reset during the same session')
        result[name + '_hz'] = delta / duration
    return result


def read_log(path):
    return path.read_text(encoding='latin1') if path.exists() else ''


def sample_client(port):
    return {'stats': query(port, 'khz_stats'),
            'state': query(port, 'khz_state', 'KHZ_STATE')}


def run_stage(cap, args, server):
    measure_seconds = getattr(args, 'measure_seconds', MEASURE_SECONDS)
    tolerance_hz = getattr(args, 'rate_tolerance_hz', RATE_TOLERANCE_HZ)
    if type(measure_seconds) is not int or not 1 <= measure_seconds <= 60 or not 0 < tolerance_hz <= RATE_TOLERANCE_HZ:
        raise ValueError('Invalid bounded measurement duration or rate tolerance')
    actual = sample_client(args.client_port)
    presentation = actual['stats'].get('presentation_state', {})
    expected_height = {'480p': 480, '720p': 720}[args.resolution]
    if actual['state'].get('map') not in (args.map, f'maps/mp/{args.map}.d3dbsp') or presentation.get('height') != expected_height:
        raise RuntimeError(f'Actual map/resolution differs from fixture: {actual["state"].get("map")}, '
                           f'{presentation.get("width")}x{presentation.get("height")}')
    if actual['state'].get('connection') != 9 or not actual['stats'].get('client_owner', {}).get('active'):
        raise RuntimeError('Actual rendered client is not active before cap stage')
    applied = query(args.client_port, f'khz_cap {cap}', 'KHZ_CAP')
    if applied.get('cap') != cap:
        raise RuntimeError(f'Client did not apply cap {cap}: {applied}')
    warm_deadline = time.monotonic() + WARM_SECONDS
    server.reset_work()
    warm_delay = warm_deadline - time.monotonic()
    if warm_delay > 0:
        time.sleep(warm_delay)
    first_server = server.sample()
    first_client = sample_client(args.client_port)
    session = first_client['state']['server_id']
    start = time.monotonic()
    measurement_started_utc = datetime.now(timezone.utc).isoformat()
    samples = [first_client]
    for index in range(1, measure_seconds + 1):
        delay = start + index - time.monotonic()
        if delay > 0:
            time.sleep(delay)
        samples.append(sample_client(args.client_port))
    measurement_finished_utc = datetime.now(timezone.utc).isoformat()
    last_server = server.sample()
    last_client = samples[-1]
    client_rates = rates(first_client['stats'], last_client['stats'],
                         ('prediction_ticks', 'prediction_steps', 'commands_sent',
                          'snapshots_received', 'gpu_presents', 'gpu_present_failures'))
    collision = collision_path(first_client['stats'], last_client['stats'], tolerance_hz)
    client_rates.update(collision)
    server_rates = rates(first_server, last_server,
                         ('ticks', 'commands_received', 'snapshots_sent'))
    initial_peers = {peer['port']: peer for peer in first_server['peers']}
    final_peers = {peer['port']: peer for peer in last_server['peers']}
    first_peer = initial_peers.get(args.client_port)
    last_peer = final_peers.get(args.client_port)
    peer_rates = {}
    alive_fraction = 0
    if first_peer and last_peer:
        duration = last_server['seconds'] - first_server['seconds']
        peer_rates = {name + '_hz': (last_peer[name] - first_peer[name]) / duration
                      for name in ('commands', 'snapshots')}
        alive_fraction = ((last_peer['playing_ticks'] - first_peer['playing_ticks']) /
                          max(1, last_server['ticks'] - first_server['ticks']))
    hz_values = ([client_rates[name + '_hz'] for name in
                  ('prediction_ticks', 'commands_sent', 'snapshots_received')]
                 + list(server_rates.values()) + list(peer_rates.values()))
    fixed_rates_pass = (bool(peer_rates) and
                        all(abs(value - 1000) <= tolerance_hz for value in hz_values))
    connection_pass = all(sample['state']['connection'] == 9
                          and sample['state']['server_id'] == session for sample in samples)
    cap_metadata_pass = all(sample['stats'].get('render_cap') == cap for sample in samples)
    gameplay_pass = connection_pass and alive_fraction >= .2 and client_rates['prediction_steps_hz'] > 0
    gpu_health_pass = client_rates['gpu_presents_hz'] > 0 and client_rates['gpu_present_failures_hz'] == 0
    # Hardware throughput is a separate goal. Low caps must obey the requested pace.
    cap_accuracy_pass = (abs(client_rates['gpu_presents_hz'] - cap) <= cap * .02 if cap in (125, 240)
                         else client_rates['gpu_presents_hz'] <= cap * 1.02 if cap else True)
    server_debt_pass = last_server['debt'] <= max(8, first_server['debt'] + 2)
    client_debt = {
        'start_debt': first_client['stats']['debt'],
        'end_debt': last_client['stats']['debt'],
        'max_sampled_debt': max(sample['stats']['debt'] for sample in samples),
        'debt_pass': last_client['stats']['debt'] <= max(8, first_client['stats']['debt'] + 2)}
    debt_pass = server_debt_pass and client_debt['debt_pass']
    work_pass = last_server['work_p99_us'] < 1000
    owner = independent_owner_path([sample['stats'] for sample in samples])
    bots = [sample.get('native_bots', {}) for sample in (first_server, last_server)]
    bot_pass = all(sample.get('active') == args.desired_bots and sample.get('humans') == 1
                   and sample.get('requested') == args.desired_bots and sample.get('script_bootstrap')
                   for sample in bots)
    passed = all((fixed_rates_pass, gameplay_pass, gpu_health_pass,
                  cap_accuracy_pass, cap_metadata_pass, debt_pass, work_pass, collision['collision_pass'], owner['passed'], bot_pass))
    return {'cap': cap, 'cap_response': applied, 'measured_seconds': time.monotonic() - start,
            'measurement_started_utc': measurement_started_utc, 'measurement_finished_utc': measurement_finished_utc,
            'warm_seconds': WARM_SECONDS, 'client': client_rates, 'server': server_rates,
            'server_peer': peer_rates, 'alive_fraction': alive_fraction,
            'fixed_rates_pass': fixed_rates_pass, 'gameplay_pass': gameplay_pass,
            'independent_owner': owner, 'independent_owner_pass': owner['passed'], 'bots_pass': bot_pass,
            'collision_pass': collision['collision_pass'],
            'gpu_health_pass': gpu_health_pass, 'cap_accuracy_pass': cap_accuracy_pass,
            'cap_metadata_pass': cap_metadata_pass,
            'work_p99_us': last_server['work_p99_us'], 'work_pass': work_pass,
            'start_debt': first_server['debt'], 'end_debt': last_server['debt'], 'debt_pass': debt_pass,
            'server_debt_pass': server_debt_pass, 'client_debt_pass': client_debt['debt_pass'],
            'client_debt': client_debt,
            'reached_1000_fps': client_rates['gpu_presents_hz'] >= 1000,
            'exceeded_1000_fps': client_rates['gpu_presents_hz'] > 1000,
            'passed': passed, 'samples': samples,
            'first_server': first_server, 'last_server': last_server}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--server-port', type=int, default=28961)
    parser.add_argument('--client-port', type=int, default=28975)
    parser.add_argument('--map', choices=('mp_killhouse', 'mp_crash'), default='mp_crash')
    parser.add_argument('--resolution', choices=('480p', '720p'), default='720p')
    parser.add_argument('--desired-bots', type=int, choices=range(16), default=15)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    server_log = root / 'runtime' / 'benchmarks' / f'server-{args.server_port}' / 'main' / 'khz_server.log'
    initial_script_errors = read_log(server_log).count('******* script runtime error *******')
    report = {'map': args.map, 'resolution': args.resolution, 'caps': CAPS,
              'rate_tolerance_hz': RATE_TOLERANCE_HZ, 'stages': [], 'errors': [],
              'scope': 'One joined live rendered client; 2 s cap warmup and 8 s sampling per stage. '
                       'Short rate tolerance is 2 Hz. Camera/input progression is continuous rather '
                       'than replayed identically. FPS throughput is reported separately from fixed '
                       'gameplay/protocol checks; this is not a long stability or deterministic replay test.'}
    start = time.monotonic()
    try:
        report['binary_sha256'] = {
            name: hashlib.sha256((root / 'runtime' / name).read_bytes()).hexdigest()
            for name in ('cod4x1khz.exe', 'cod4x1khz_server.exe')}
        server = ServerQueries(args.server_port)
        for cap in CAPS:
            stage = run_stage(cap, args, server)
            report['stages'].append(stage)
            print(f'Cap {cap or "uncapped"}: {stage["client"]["gpu_presents_hz"]:.1f} FPS, '
                  f'{stage["server"]["ticks_hz"]:.3f} server Hz, passed={stage["passed"]}', flush=True)
            if not stage['passed']:
                report['errors'].append(f'Cap {cap} failed one or more fixed logic, collision, cap or GPU checks')
        report['new_script_errors'] = max(0, read_log(server_log).count(
            '******* script runtime error *******') - initial_script_errors)
        if report['new_script_errors']:
            report['errors'].append('Server reported new script runtime errors')
    except (OSError, ValueError, RuntimeError, KeyError, ZeroDivisionError) as exc:
        report['errors'].append(str(exc))
    finally:
        report['elapsed_seconds'] = time.monotonic() - start
        report['collision_pass'] = (len(report['stages']) == len(CAPS)
                                    and all(stage['collision_pass'] for stage in report['stages']))
        report['passed'] = len(report['stages']) == len(CAPS) and not report['errors']
        uncapped = next((stage for stage in report['stages'] if stage['cap'] == 0), None)
        report['uncapped_fps_goal_pass'] = bool(uncapped and uncapped['exceeded_1000_fps'])
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(report, indent=2) + '\n', encoding='utf8')
        print(json.dumps({key: report[key] for key in ('passed', 'collision_pass', 'uncapped_fps_goal_pass',
                                                       'elapsed_seconds', 'errors')}, indent=2), flush=True)
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
