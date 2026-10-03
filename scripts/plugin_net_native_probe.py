"""Short owned SDK2 address/admission/IP-ban probe; never builds or deploys.

Uses one isolated dedicated server and one headless human for at most35s.
Native transport remains IPv4; parsed IPv6 does not prove UDP/game parity.
"""
import hashlib
import argparse
import json
from pathlib import Path
import re
import socket
import subprocess
import time
from monitor import query
from plugin_fixture_archive import bind_archive

PROJECT = Path(__file__).resolve().parent.parent
RUNTIME = PROJECT / 'runtime'
SERVER_PORT, CLIENT_PORT = 28982, 28983


def main(maximum_seconds=35, build_archive=None):
    output = PROJECT / 'results' / ('plugin-net-native-' + time.strftime('%Y%m%d-%H%M%S') + f'-{time.time_ns() % 1000000:06d}')
    output.mkdir()
    result = {'scope': 'Actual SDK2 public32 numeric/mask/format/LAN exports, IPv4 native callbacks/reserved admission and transient IP-ban text; DNS/IPv6 UDP/IPv6 bans pending',
              'cases': [], 'commands': [], 'processes': [], 'sha256': {}}
    owned = []
    started = time.monotonic()
    deadline = started + maximum_seconds
    last_rcon = 0
    log = RUNTIME / 'benchmarks' / f'server-{SERVER_PORT}' / 'main/khz_server.log'
    prior = log.read_bytes() if log.exists() else b''

    def check(name, okay, evidence):
        result['cases'].append({'name': name, 'passed': bool(okay), 'evidence': evidence})

    def launch(name, arguments):
        stream = (output / (name + '-stdio.log')).open('wb')
        process = subprocess.Popen([str(RUNTIME / (name + '.exe')), *arguments], cwd=RUNTIME, stdout=stream, stderr=subprocess.STDOUT,
                                   creationflags=subprocess.CREATE_NO_WINDOW)
        owned.append((process, stream))
        result['processes'].append({'pid': process.pid, 'name': name, 'arguments': arguments})
        (output / 'processes.json').write_text(json.dumps(result['processes'], indent=2))
        return process

    def rcon(command):
        nonlocal last_rcon
        time.sleep(max(0, 1.08 - (time.monotonic() - last_rcon)))
        chunks = []
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
            sock.settimeout(.4)
            sock.sendto(b'\xff' * 4 + ('rcon local-khz-benchmark ' + command).encode('ascii'), ('127.0.0.1', SERVER_PORT))
            last_rcon = time.monotonic()
            try:
                while True:
                    packet, sender = sock.recvfrom(65536)
                    if sender != ('127.0.0.1', SERVER_PORT):
                        continue
                    if packet.startswith(b'\xff' * 4):
                        packet = packet[4:]
                    if packet.startswith(b'print\n'):
                        packet = packet[6:]
                    chunks.append(packet.decode('latin1'))
            except (socket.timeout, ConnectionResetError):
                pass
        reply = ''.join(chunks)
        result['commands'].append({'command': command, 'reply': reply})
        return reply

    def record(text, marker):
        lines = [line[line.index(marker):] for line in text.splitlines() if marker in line]
        return lines[-1] if lines else ''

    try:
        for port in (SERVER_PORT, CLIENT_PORT):
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as reserve:
                reserve.bind(('127.0.0.1', port))
        for name in ('runtime/cod4x1khz_server.exe', 'runtime/cod4x1khz_bench.exe', 'runtime/plugins/khz_regression.dll',
                     'engine/src/khz/khz_plugins.cpp', 'engine/src/khz/khz_plugin_net.inl', 'engine/src/khz/khz_plugin_net_address.cpp',
                     'sdk/include/pinc.h', 'sdk/include/public_net_address.h', 'sdk/include/public_http_api.h',
                     'sdk/tests/native_net_address_probe.inc', 'sdk/tests/regression_plugin.c', 'scripts/plugin_net_native_probe.py',
                     'engine/src/khz/khz_admin.cpp', 'engine/src/khz/khz_admin_server.cpp'):
            path = PROJECT / name
            result['sha256'][str(path)] = hashlib.sha256(path.read_bytes()).hexdigest()
        build = bind_archive(PROJECT, build_archive, result,
            ('runtime/cod4x1khz_server.exe', 'runtime/cod4x1khz_bench.exe', 'runtime/plugins/khz_regression.dll'),
            ('engine/src/khz/khz_plugins.cpp', 'engine/src/khz/khz_plugin_net.inl',
             'engine/src/khz/khz_plugin_net_address.cpp', 'engine/src/khz/khz_admin.cpp',
             'engine/src/khz/khz_admin_server.cpp', 'sdk/include/pinc.h',
             'sdk/include/public_net_address.h', 'sdk/include/public_http_api.h',
             'sdk/tests/native_net_address_probe.inc', 'sdk/tests/regression_plugin.c'))
        server = launch('cod4x1khz_server', ['+set', 'dedicated', '1', '+set', 'net_ip', '127.0.0.1', '+set', 'net_port', str(SERVER_PORT),
            '+set', 'khz_benchmark', '1', '+set', 'khz_test_controls', '1', '+exec', 'server.cfg', '+set', 'khz_bots', '0',
            '+set', 'khz_bot_count', '0', '+set', 'sv_maxclients', '1', '+set', 'sv_privateClients', '1',
            '+set', 'banlist_maxipbantime', '240', '+loadPlugin', 'khz_regression', '+set', 'khz_plugin_tests', '1',
            '+set', 'khz_fixture_net_allow_reserved', '1', '+map', 'mp_killhouse'])
        while time.monotonic() < deadline - min(18, maximum_seconds * .55):
            if server.poll() is not None:
                raise RuntimeError('Owned server exited before readiness')
            reply = rcon('khz_fixture_net')
            if 'KHZ_PLUGIN_NET_FIXTURE' in reply:
                break
        else:
            raise TimeoutError('Actual rebuilt SDK2 fixture did not become ready')
        native = record(reply, 'KHZ_PLUGIN_NET_FIXTURE')
        fields = dict((key, int(value)) for key, value in re.findall(r'(cases|failures|sdk)=(\d+)', native))
        check('actual numeric/mask/format/LAN exports through rebuilt C DLL', fields.get('cases') == 40 and fields.get('failures') == 0 and fields.get('sdk') == 2, native)
        ipban = rcon('khz_fixture_net_ipban')
        native = record(ipban, 'KHZ_PLUGIN_NET_IPBAN')
        fields = dict((key, int(value)) for key, value in re.findall(r'(cases|failures|cleanup)=(\d+)', native))
        check('native public32 IPv4 IP-ban roundtrip/time/bounds/isolation/removal', fields.get('cases') == 8 and fields.get('failures') == 0 and fields.get('cleanup') == 1, native)
        client = launch('cod4x1khz_bench', ['+set', 'net_port', str(CLIENT_PORT), '+set', 'khz_test_controls', '1', '+exec', 'client.cfg',
            '+set', 'com_introPlayed', '1', '+set', 'name', 'sdk2-native-address', '+connect', f'127.0.0.1:{SERVER_PORT}'])
        state = {}
        while time.monotonic() < deadline - min(7, maximum_seconds * .2):
            if client.poll() is not None:
                raise RuntimeError('Owned headless human exited before admission')
            try:
                state = query(CLIENT_PORT, 'khz_state', 'KHZ_STATE', .35)
                if state.get('connection') == 9:
                    break
            except (OSError, TimeoutError):
                pass
            time.sleep(.05)
        check('actual private-slot human admission', state.get('connection') == 9, state)
        admission = record(rcon('khz_fixture_net_admission'), 'KHZ_PLUGIN_NET_ADMISSION')
        fields = dict((key, int(value)) for key, value in re.findall(r'(connects|connect_layout|reserved|reserved_layout|grants)=(\d+)', admission))
        check('actual native IPv4 connect callback translated to public32', fields.get('connects', 0) > 0 and fields.get('connect_layout') == fields.get('connects') and 'connect=127.0.0.1:' in admission, admission)
        check('actual native reserved callback translated/grant honored', fields.get('reserved', 0) > 0 and fields.get('reserved_layout') == fields.get('reserved') and fields.get('grants', 0) > 0 and 'reserved_address=127.0.0.1:' in admission, admission)
        # Leave enough cleanup budget: independent unload state is checked after
        # all transient fixture IPs were removed and the owner callbacks ran.
        rcon('unloadPlugin khz_regression')
        stats = query(SERVER_PORT, 'rcon local-khz-benchmark khz_json', 'KHZ_JSON', .6)
        last_rcon = time.monotonic()
        check('SDK2 module unload after actual callback scopes', stats.get('plugins', {}).get('loaded') == 0, stats.get('plugins', {}))
        check('deployed server/client/DLL, compiled address sources and fixture package match selected build archive',
              True, {'build': build, 'archive': result['build_archive']})
    except Exception as error:
        result['error'] = str(error)
    finally:
        for process, stream in reversed(owned):
            if process.poll() is None:
                try:
                    if 'bench' in Path(process.args[0]).name:
                        query(CLIENT_PORT, 'khz_ui quit', 'KHZ_UI', .25)
                    else:
                        rcon('quit')
                    process.wait(timeout=1.5)
                except (OSError, TimeoutError, subprocess.TimeoutExpired):
                    process.terminate()
                    try:
                        process.wait(timeout=1.5)
                    except subprocess.TimeoutExpired:
                        process.kill(); process.wait(timeout=1.5)
            stream.close()
        data = log.read_bytes() if log.exists() else b''
        if prior and data.startswith(prior):
            data = data[len(prior):]
        (output / 'native-server.log').write_bytes(data)
        result['owned_processes_exited'] = bool(owned) and all(process.poll() is not None for process, stream in owned)
        result['elapsed_seconds'] = time.monotonic() - started
        result['maximum_seconds'] = maximum_seconds
        if result['elapsed_seconds'] > maximum_seconds:
            result['error'] = result.get('error', 'Owned fixture exceeded requested time bound')
        result['passed'] = not result.get('error') and result['owned_processes_exited'] and bool(result['cases']) and all(case['passed'] for case in result['cases'])
        (output / 'result.json').write_text(json.dumps(result, indent=2))
        print(json.dumps({'passed': result['passed'], 'elapsed_seconds': result['elapsed_seconds'], 'error': result.get('error')}, indent=2))
        print(output / 'result.json')
    return 0 if result['passed'] else 1


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--seconds', type=float, default=35)
    parser.add_argument('--build-archive', required=True, help='Project-local coherent build archive; checked before launch')
    args = parser.parse_args()
    if not 15 <= args.seconds <= 35:
        parser.error('--seconds must be15..35')
    raise SystemExit(main(args.seconds, args.build_archive))
