"""Short checks against a live map, real command queues and weapon rewind."""
import argparse
import json
from pathlib import Path
import socket
import time
from monitor import query


def rcon(command):
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.sendto(b'\xff' * 4 + ('rcon local-khz-benchmark ' + command).encode('ascii'), ('127.0.0.1', 28961))
    time.sleep(1.2)  # stock RCON limit is one request per second per address


def server():
    result = query(28961, 'rcon local-khz-benchmark khz_json')
    time.sleep(1.1)
    return result


def state():
    return query(28974, 'khz_state', 'KHZ_STATE')


def wait_active(previous_id=None):
    until = time.monotonic() + 25
    latest = {}
    while time.monotonic() < until:
        try:
            latest = state()
            if (latest['connection'] == 9 and latest['pm_type'] == 0
                    and not (latest.get('other_flags', 0) & 2)
                    and latest['weapon'] and latest['server_id'] != previous_id):
                return latest
        except (OSError, ValueError):
            pass
        time.sleep(.2)
    raise RuntimeError(f'Client did not reach independent active gameplay: {latest}')


def recovery_rates(seconds=5):
    first = query(28974, 'khz_stats')
    time.sleep(seconds)
    last = query(28974, 'khz_stats')
    duration = last['seconds'] - first['seconds']
    return {name: (last[name] - first[name]) / duration
            for name in ('prediction_ticks', 'commands_sent', 'snapshots_received')}


parser = argparse.ArgumentParser()
parser.add_argument('--phase', choices=['transition', 'reconnect'], required=True)
parser.add_argument('--map', choices=['mp_killhouse', 'mp_crash'], default='mp_killhouse')
parser.add_argument('--output', type=Path, required=True)
args = parser.parse_args()
report = json.loads(args.output.read_text(encoding='utf8')) if args.phase == 'reconnect' else {'checks': {}, 'errors': []}
try:
    initial = wait_active()
    if args.phase == 'transition':
        time.sleep(.4)  # populate the full rewind range
        rewind = query(28961, 'rcon local-khz-benchmark khz_rewind_test', 'KHZ_REWIND')
        time.sleep(1.1)
        report['checks']['rewind'] = rewind
        assert rewind['passed'], 'Historical body rewind or exact restoration failed'
        physics = query(28961, 'rcon local-khz-benchmark khz_physics_test', 'KHZ_PHYSICS')
        time.sleep(1.1)
        report['checks']['physics'] = physics
        assert physics['passed'], 'Elapsed-time turret or projectile checks failed'
        before = server()
        stall = query(28961, 'rcon local-khz-benchmark khz_stall 50', 'KHZ_STALL')
        time.sleep(1.1)
        after = server()
        report['checks']['scheduler_stall'] = {'injected': stall, 'before': before, 'after': after}
        assert stall['debt'] >= 45 and after['debt'] <= 2, 'Stall debt did not recover'
        assert after['ticks'] > before['ticks'] + 1000, 'Simulation did not continue'
        report['checks']['stall_recovery_rates'] = recovery_rates()
        assert all(999 <= value <= 1001 for value in report['checks']['stall_recovery_rates'].values()), 'Rates did not recover after stall'
        rcon('map ' + ('mp_crash' if args.map == 'mp_killhouse' else 'mp_killhouse'))
        changed = wait_active(initial['server_id'])
        report['checks']['map_change'] = {'before': initial, 'after': changed}
        report['checks']['map_recovery_rates'] = recovery_rates()
        assert all(999 <= value <= 1001 for value in report['checks']['map_recovery_rates'].values()), 'Rates did not recover after map load'
        rcon('map_restart')
        restarted = wait_active(changed['server_id'])
        report['checks']['map_restart'] = {'before': changed, 'after': restarted}
        rcon('clientkick 0')
        until = time.monotonic() + 5
        disconnected = state()
        while disconnected['connection'] == 9 and time.monotonic() < until:
            time.sleep(.2)
            disconnected = state()
        report['checks']['disconnect'] = disconnected
        assert disconnected['connection'] != 9, 'Server kick did not disconnect the client'
    else:
        report['checks']['reconnect'] = initial
        report['checks']['reconnect_rates'] = recovery_rates()
        assert all(999 <= value <= 1001 for value in report['checks']['reconnect_rates'].values()), 'Reconnected client did not recover 1 kHz'
        peers = server()['peers']
        report['checks']['reconnected_peers'] = peers
        assert len(peers) == 1 and peers[0]['port'] == 28974, 'The previous connection left an active peer behind'
except (AssertionError, RuntimeError, OSError, ValueError) as exc:
    report['errors'].append(str(exc))
report['passed'] = not report['errors']
report['scope'] = 'live bounded stall recovery, historical body rewind/restore, map load/restart and kicked-client replacement; historical bone poses and hits need separate tests'
args.output.parent.mkdir(parents=True, exist_ok=True)
args.output.write_text(json.dumps(report, indent=2), encoding='utf8')
print(json.dumps(report, indent=2), flush=True)
raise SystemExit(0 if report['passed'] else 1)
