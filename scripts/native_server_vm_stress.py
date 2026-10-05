"""Bounded native server combat/VM restart regression, without a renderer."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import socket
import subprocess
import time
from monitor import query

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--seconds', type=int, default=120)
parser.add_argument('--bots', type=int, default=15)
parser.add_argument('--output', required=True)
args = parser.parse_args()
if not 45 <= args.seconds <= 600 or not 2 <= args.bots <= 15:
    parser.error('Use 45..600 seconds and 2..15 bots')
root = Path(__file__).resolve().parent.parent
server = root / 'runtime/x64/cod4x1khz_server.exe'
probe = root / 'build/x64/Release/Release/khz_launch_probe.exe'
prefix = root / args.output
prefix.parent.mkdir(parents=True, exist_ok=True)
port = 28987
with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as reservation:
    reservation.bind(('127.0.0.1', port))
arguments = (f'+set net_port {port} +set dedicated 2 +set khz_benchmark 1 +set khz_test_controls 1 '
             f'+set khz_bots 1 +set khz_bot_count {args.bots} +set g_gametype dm '
             '+set rcon_password local-khz-benchmark +set logfile 2 +set sv_lan 1 +map mp_crash')
report = {'server_sha256': hashlib.sha256(server.read_bytes()).hexdigest(), 'arguments': arguments,
          'samples': [], 'transitions': [], 'passed': False}
started = time.monotonic()
env = dict(os.environ, KHZ_PROBE_SECONDS=str(args.seconds + 35), KHZ_PROBE_HIDE_WINDOW='1')
env.pop('KHZ_WATCH_HUNK', None)
env.pop('KHZ_WATCH_SYMBOL', None)
def send(command):
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as connection:
        connection.sendto(b'\xff' * 4 + ('rcon local-khz-benchmark ' + command).encode('ascii'), ('127.0.0.1', port))
with prefix.with_suffix('.log').open('w') as log:
    owned = subprocess.Popen([str(probe), str(server), str(root), str(server.parent), arguments],
        cwd=root, env=env, stdout=log, stderr=subprocess.STDOUT, creationflags=subprocess.CREATE_NO_WINDOW)
    try:
        last_sample = 0
        transition = 0
        while time.monotonic() - started < args.seconds:
            if owned.poll() is not None:
                raise RuntimeError(f'Owned server exited during combat: {owned.returncode}')
            elapsed = time.monotonic() - started
            if elapsed > 8 and elapsed >= last_sample + 2:
                last_sample = elapsed
                try:
                    value = query(port, 'rcon local-khz-benchmark khz_json', 'KHZ_JSON', .8)
                    inspected = query(port, 'rcon local-khz-benchmark khz_bot_inspect', 'KHZ_BOT_INSPECTION', .8)
                    report['samples'].append({'elapsed': elapsed, 'stats': value, 'inspection': inspected})
                except TimeoutError:
                    pass
            if elapsed > (transition + 1) * args.seconds / 3 and transition < 2:
                command = 'fast_restart' if transition == 0 else 'map mp_killhouse'
                send(command)
                report['transitions'].append({'elapsed': elapsed, 'command': command})
                print(json.dumps(report['transitions'][-1]), flush=True)
                transition += 1
            time.sleep(.2)
        ready = [s for s in report['samples'] if s['stats'].get('native_bots', {}).get('active') == args.bots
                 and s['inspection'].get('complete') is True]
        if len(ready) < 10:
            raise ValueError('No sustained native bot combat across the restart fixture')
        report['ready_samples'] = len(ready)
        sessions = {}
        spawns = {}
        spawn_changes = 0
        for sample in ready:
            inspected = sample['inspection']
            session = inspected['session']
            sessions[session] = sessions.get(session, 0) + 1
            for slot in inspected['slots']:
                key = (session, slot['slot'], slot['generation'])
                previous = spawns.get(key)
                if previous is not None and slot['last_spawn_time'] > previous:
                    spawn_changes += 1
                spawns[key] = slot['last_spawn_time']
        report['session_sample_counts'] = sessions
        report['observed_spawn_changes'] = spawn_changes
        if len(sessions) != 3 or min(sessions.values()) < 3 or not spawn_changes:
            raise ValueError('Native combat/respawns were not sustained across all three sessions')
        send('quit')
        report['exit_code'] = owned.wait(timeout=10)
        if report['exit_code'] != 0:
            raise ValueError('Owned server did not exit cleanly')
        report['passed'] = True
    except Exception as error:
        report['error'] = str(error)
    finally:
        if owned.poll() is None:
            send('quit')
            try:
                owned.wait(timeout=5)
            except subprocess.TimeoutExpired:
                owned.terminate()
                owned.wait(timeout=5)
        report['exit_code'] = owned.returncode
        report['elapsed'] = time.monotonic() - started
        prefix.with_suffix('.json').write_text(json.dumps(report, indent=2) + '\n')
print(json.dumps({k: v for k, v in report.items() if k not in ('samples', 'arguments')}))
raise SystemExit(0 if report['passed'] else 1)
