"""Verify deployed SDK synchronous RCON output and queued tick/worker logs.

Dedicated-only, zero bots, owned port28982/process/listener state, <=20 seconds.
This never compiles or deploys the engine.
"""
import hashlib
import json
from pathlib import Path
import re
import socket
import subprocess
import time

PROJECT = Path(__file__).resolve().parent.parent
RUNTIME = PROJECT / 'runtime'
PORT = 28982


def main():
    output = PROJECT / 'results' / ('plugin-print-native-' + time.strftime('%Y%m%d-%H%M%S') + f'-{time.time_ns() % 1000000:06d}')
    output.mkdir()
    log = RUNTIME / 'benchmarks' / f'server-{PORT}' / 'main/khz_server.log'
    before = log.read_bytes() if log.exists() else b''
    result = {'scope': 'Actual rebuilt Printf/Warning/Error control redirect and worker/tick journal paths; no broad SDK parity claim',
              'cases': [], 'commands': [], 'sha256': {}}
    process = stdio = None
    started = time.monotonic()
    deadline = started + 20
    last_rcon = 0

    def fresh():
        current = log.read_bytes() if log.exists() else b''
        return current[len(before):] if before and current.startswith(before) else current

    def rcon(text):
        nonlocal last_rcon
        time.sleep(max(0, 1.08 - (time.monotonic() - last_rcon)))
        chunks = []
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as udp:
            udp.settimeout(.5)
            udp.sendto(b'\xff' * 4 + ('rcon local-khz-benchmark ' + text).encode('ascii'), ('127.0.0.1', PORT))
            last_rcon = time.monotonic()
            try:
                while True:
                    packet, sender = udp.recvfrom(65536)
                    if sender != ('127.0.0.1', PORT):
                        continue
                    if packet.startswith(b'\xff' * 4):
                        packet = packet[4:]
                    if packet.startswith(b'print\n'):
                        packet = packet[6:]
                    chunks.append(packet.decode('latin1'))
            except (socket.timeout, ConnectionResetError):
                pass
        reply = ''.join(chunks)
        result['commands'].append({'command': text, 'reply': reply})
        return reply

    def check(name, okay, evidence):
        result['cases'].append({'name': name, 'passed': bool(okay), 'evidence': evidence})

    try:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as reserve:
            reserve.bind(('127.0.0.1', PORT))
        for name in ['runtime/cod4x1khz_server.exe', 'runtime/plugins/khz_regression.dll',
                     'engine/src/khz/khz_plugins.cpp', 'sdk/tests/regression_plugin.c',
                     'sdk/tests/native_print_probe.inc', 'scripts/plugin_print_native_probe.py']:
            path = PROJECT / name
            result['sha256'][str(path)] = hashlib.sha256(path.read_bytes()).hexdigest()
        arguments = [str(RUNTIME / 'cod4x1khz_server.exe'), '+set', 'dedicated', '1',
                     '+set', 'net_ip', '127.0.0.1', '+set', 'net_port', str(PORT),
                     '+set', 'khz_benchmark', '1', '+set', 'khz_test_controls', '1',
                     '+exec', 'server.cfg', '+set', 'khz_bots', '0', '+set', 'khz_bot_count', '0',
                     '+loadPlugin', 'khz_regression', '+set', 'khz_plugin_tests', '1', '+map', 'mp_killhouse']
        stdio = (output / 'server-stdio.log').open('wb')
        process = subprocess.Popen(arguments, cwd=RUNTIME, stdout=stdio, stderr=subprocess.STDOUT,
                                   creationflags=subprocess.CREATE_NO_WINDOW)
        result['process'] = {'pid': process.pid, 'arguments': arguments}
        (output / 'process.json').write_text(json.dumps(result['process'], indent=2))
        ready = ''
        while time.monotonic() < deadline - 8:
            if process.poll() is not None:
                raise RuntimeError('Owned server exited before readiness')
            ready = rcon('khz_fixture_report')
            if 'KHZ_PLUGIN_FIXTURE frames=' in ready:
                break
        else:
            raise TimeoutError('Synchronous plugin report did not arrive in RCON readiness scope')
        direct = rcon('khz_fixture_print')
        expected = ['KHZ_PLUGIN_PRINT_FIXTURE direct=1 main=1 tick=0\n',
                    '^3Warning: KHZ_PLUGIN_PRINT_FIXTURE direct_warning=1\n',
                    '^1Error: KHZ_PLUGIN_PRINT_FIXTURE direct_error=1\n']
        for marker in expected:
            check('synchronous exact redirect ' + marker.partition('=')[0], marker in direct and direct.count(marker) == 1, direct)
        check('worker/tick output stays outside control redirect', 'worker=1' not in direct and 'tick=1' not in direct, direct)
        completion = rcon('khz_fixture_print_report')
        check('pinned worker owner completion', 'worker=1 completions=1 owner=1 tick=1' in completion, completion)
        data = fresh()
        text = data.decode('latin1')
        expected_logs = ['KHZ_PLUGIN_PRINT_FIXTURE worker=1 main=0',
                         'Warning: KHZ_PLUGIN_PRINT_FIXTURE worker_warning=1',
                         'Error: KHZ_PLUGIN_PRINT_FIXTURE worker_error=1',
                         'KHZ_PLUGIN_PRINT_FIXTURE tick=1 authoritative=1',
                         'Warning: KHZ_PLUGIN_PRINT_FIXTURE tick_warning=1',
                         'Error: KHZ_PLUGIN_PRINT_FIXTURE tick_error=1']
        for marker in expected_logs:
            check('queued actual native log ' + marker, marker in text, marker)
        # Native Com_PrintMessage strips the leading color in its disk log,
        # while stderr and the synchronous redirect retain exact raw text.
        raw = (output / 'server-stdio.log').read_bytes().decode('latin1')
        for marker in ['^3Warning: KHZ_PLUGIN_PRINT_FIXTURE worker_warning=1',
                       '^1Error: KHZ_PLUGIN_PRINT_FIXTURE worker_error=1',
                       '^3Warning: KHZ_PLUGIN_PRINT_FIXTURE tick_warning=1',
                       '^1Error: KHZ_PLUGIN_PRINT_FIXTURE tick_error=1']:
            check('queued raw stderr prefix ' + marker, marker in raw, marker)
        (output / 'native-server.log').write_bytes(data)
        rcon('unloadPlugin khz_regression')
        stats_reply = rcon('khz_json')
        match = re.search(r'KHZ_JSON\s+(\{[^\r\n]*\})', stats_reply)
        stats = json.loads(match.group(1)) if match else {}
        check('fixture unloaded after worker completion', stats.get('plugins', {}).get('loaded') == 0, stats.get('plugins', {}))
    except Exception as error:
        result['error'] = str(error)
    finally:
        if process:
            if process.poll() is None:
                try:
                    rcon('quit')
                    process.wait(timeout=2)
                except (OSError, subprocess.TimeoutExpired):
                    process.terminate()
                    try:
                        process.wait(timeout=2)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait(timeout=2)
            result['owned_process_exited'] = process.poll() is not None
        if stdio:
            stdio.close()
        result['elapsed_seconds'] = time.monotonic() - started
        result['passed'] = not result.get('error') and result.get('owned_process_exited') and bool(result['cases']) and all(case['passed'] for case in result['cases'])
        (output / 'result.json').write_text(json.dumps(result, indent=2))
        print(json.dumps({'passed': result['passed'], 'elapsed_seconds': result['elapsed_seconds'], 'error': result.get('error')}, indent=2))
        print(output / 'result.json')
    return 0 if result['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
