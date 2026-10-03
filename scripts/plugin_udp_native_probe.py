"""Bounded owned native SDK DNS/IPv4 UDP/callback/injection proof.

Never builds/deploys; server28982 and two loopback listeners28984/28985.
IPv6 DNS is a parsed/resolved capability, not native IPv6 game transport.
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
SERVER_PORT, DIRECT_PORT, ALTERNATE_PORT = 28982, 28984, 28985


def main(build_archive):
    output = PROJECT / 'results' / ('plugin-udp-native-' + time.strftime('%Y%m%d-%H%M%S') + f'-{time.time_ns() % 1000000:06d}')
    output.mkdir()
    result = {'scope': 'Actual bounded pinned DNS, configured-source-port IPv4 UDP, native mutable/veto callbacks and queued protocol/proof-validated injection',
              'cases': [], 'commands': [], 'packets': [], 'processes': [], 'sha256': {}, 'ipv6_game_transport_proven': False}
    started = time.monotonic()
    deadline = started + 35
    owned = []
    last_rcon = 0
    log = RUNTIME / 'benchmarks' / f'server-{SERVER_PORT}' / 'main/khz_server.log'
    prior = log.read_bytes() if log.exists() else b''

    def check(name, okay, evidence):
        result['cases'].append({'name': name, 'passed': bool(okay), 'evidence': evidence})

    def packet(sock, maximum=.4):
        sock.settimeout(maximum)
        try:
            data, sender = sock.recvfrom(65536)
            record = {'receiver': sock.getsockname()[1], 'sender': list(sender), 'hex': data.hex()}
            result['packets'].append(record)
            return data, sender
        except (socket.timeout, ConnectionResetError):
            return b'', None

    def rcon(command):
        nonlocal last_rcon
        time.sleep(max(0, 1.08 - (time.monotonic() - last_rcon)))
        chunks = []
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as control:
            control.bind(('127.0.0.1', 0))
            control.settimeout(.25)
            control.sendto(b'\xff' * 4 + ('rcon local-khz-benchmark ' + command).encode('ascii'), ('127.0.0.1', SERVER_PORT))
            last_rcon = time.monotonic()
            try:
                while True:
                    data, sender = control.recvfrom(65536)
                    if sender != ('127.0.0.1', SERVER_PORT):
                        continue
                    if data.startswith(b'\xff' * 4):
                        data = data[4:]
                    if data.startswith(b'print\n'):
                        data = data[6:]
                    chunks.append(data.decode('latin1'))
            except (socket.timeout, ConnectionResetError):
                pass
        reply = ''.join(chunks)
        result['commands'].append({'command': command, 'reply': reply})
        return reply

    try:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as reserve:
            reserve.bind(('127.0.0.1', SERVER_PORT))
        primary = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        alternate = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        primary.bind(('127.0.0.1', DIRECT_PORT))
        alternate.bind(('127.0.0.1', ALTERNATE_PORT))
        for name in ('runtime/cod4x1khz_server.exe', 'runtime/plugins/khz_regression.dll', 'engine/src/khz/khz_plugins.cpp',
                     'engine/src/khz/khz_plugin_net.inl', 'engine/src/khz/khz_plugin_dns.cpp', 'engine/src/win32/win_net.cpp',
                     'engine/src/qcommon/net_chan_mp.cpp', 'engine/src/qcommon/common.cpp', 'native/plugin_packet_mailbox.hpp',
                     'sdk/include/pinc.h', 'sdk/include/public_net_api.h', 'sdk/tests/native_udp_probe.inc',
                     'sdk/tests/regression_plugin.c', 'scripts/plugin_udp_native_probe.py'):
            path = PROJECT / name
            result['sha256'][str(path)] = hashlib.sha256(path.read_bytes()).hexdigest()
        build = bind_archive(PROJECT, build_archive, result,
            ('runtime/cod4x1khz_server.exe', 'runtime/plugins/khz_regression.dll'),
            ('engine/src/khz/khz_plugins.cpp', 'engine/src/khz/khz_plugin_net.inl',
             'engine/src/khz/khz_plugin_dns.cpp', 'engine/src/khz/khz_plugin_net_address.cpp',
             'engine/src/win32/win_net.cpp', 'engine/src/qcommon/net_chan_mp.cpp',
             'engine/src/qcommon/common.cpp', 'native/plugin_packet_mailbox.hpp',
             'sdk/include/pinc.h', 'sdk/include/public_net_api.h',
             'sdk/include/public_net_address.h', 'sdk/tests/native_udp_probe.inc',
             'sdk/tests/regression_plugin.c'))
        check('deployed server/DLL, compiled networking sources and fixture package match selected build archive',
              True, {'build': build, 'archive': result['build_archive']})
        stream = (output / 'server-stdio.log').open('wb')
        arguments = ['+set', 'dedicated', '1', '+set', 'net_ip', '127.0.0.1', '+set', 'net_port', str(SERVER_PORT),
            '+set', 'khz_benchmark', '1', '+set', 'khz_test_controls', '1', '+exec', 'server.cfg',
            '+set', 'khz_bots', '0', '+set', 'khz_bot_count', '0', '+set', 'sv_maxclients', '16',
            '+loadPlugin', 'khz_regression', '+set', 'khz_plugin_tests', '1', '+map', 'mp_killhouse']
        server = subprocess.Popen([str(RUNTIME / 'cod4x1khz_server.exe'), *arguments], cwd=RUNTIME,
            stdout=stream, stderr=subprocess.STDOUT, creationflags=subprocess.CREATE_NO_WINDOW)
        owned.append((server, stream))
        result['processes'].append({'pid': server.pid, 'name': 'cod4x1khz_server', 'arguments': arguments})
        (output / 'processes.json').write_text(json.dumps(result['processes'], indent=2))
        while time.monotonic() < deadline - 24:
            if server.poll() is not None:
                raise RuntimeError('Owned native server exited before readiness')
            report = rcon('khz_fixture_udp report')
            if 'KHZ_UDP_REPORT' in report:
                break
        else:
            raise TimeoutError('New native UDP fixture not ready')

        direct = rcon(f'khz_fixture_udp direct 127.0.0.1:{DIRECT_PORT}')
        fields = dict((name, int(value)) for name, value in re.findall(r'(cases|failures|socket)=(-?\d+)', direct))
        data, sender = packet(primary)
        check('direct kernel UDP from configured server source port and copied binary bytes', data == b'KHZ_UDP_DIRECT_V1' and sender == ('127.0.0.1', SERVER_PORT), {'data': data.hex(), 'sender': sender})
        check('public direct send actual success and bounded invalid arguments', fields.get('cases') == 7 and fields.get('failures') == 0 and fields.get('socket') not in (None, 0, -1), direct)
        duplicate, duplicate_sender = packet(primary, .15)
        check('direct UDP delivered exactly once despite invalid follow-up calls', not duplicate, {'data': duplicate.hex(), 'sender': duplicate_sender})

        rcon(f'khz_fixture_udp worker localhost:{DIRECT_PORT}')
        data, sender = packet(primary, 2)
        check('pinned worker actual localhost DNS and UDP preserves native socket source', data == b'KHZ_UDP_WORKER_V1' and sender == ('127.0.0.1', SERVER_PORT), {'data': data.hex(), 'sender': sender})

        primary.sendto(b'\xff' * 4 + b'getinfo KHZ_UDP_RX_DROP', ('127.0.0.1', SERVER_PORT))
        data, sender = packet(primary)
        check('receive veto prevents real native query delivery', not data, {'data': data.hex(), 'sender': sender})
        primary.sendto(b'\xff' * 4 + b'getinfo KHZ_UDP_RX_EDIT', ('127.0.0.1', SERVER_PORT))
        data, sender = packet(primary)
        check('receive byte mutation reaches native packet parser', b'\\challenge\\KHZ_UDP_RX_PASS' in data and sender == ('127.0.0.1', SERVER_PORT), data.decode('latin1'))
        primary.sendto(b'\xff' * 4 + b'getinfo KHZ_UDP_RX_ADDRESS', ('127.0.0.1', SERVER_PORT))
        data, sender = packet(alternate)
        check('receive public32 endpoint mutation affects native reply routing', b'\\challenge\\KHZ_UDP_RX_ADDRESS' in data and sender == ('127.0.0.1', SERVER_PORT), data.decode('latin1'))
        duplicate, duplicate_sender = packet(primary, .15)
        check('mutated receive endpoint produces no reply to original endpoint', not duplicate, {'data': duplicate.hex(), 'sender': duplicate_sender})

        dropped = rcon('khz_fixture_udp drop')
        check('send veto suppresses actual RCON datagram', not dropped, dropped)
        edited = rcon('khz_fixture_udp edit')
        check('send payload mutation reaches actual recipient', 'KHZ_UDP_TX_EDIT B' in edited and 'KHZ_UDP_TX_EDIT A' not in edited, edited)
        rerouted = rcon('khz_fixture_udp address')
        data, sender = packet(alternate)
        check('send public32 endpoint mutation retargets real server datagram', not rerouted and b'KHZ_UDP_TX_ADDRESS' in data and sender == ('127.0.0.1', SERVER_PORT), {'reply': rerouted, 'data': data.decode('latin1')})
        duplicate, duplicate_sender = packet(alternate, .15)
        check('mutated send endpoint receives a single native datagram', not duplicate, {'data': duplicate.hex(), 'sender': duplicate_sender})

        rcon(f'khz_fixture_udp inject 127.0.0.1:{DIRECT_PORT}')
        data, sender = packet(primary)
        check('injection owns stack byte/address copy and reaches native parser', b'\\challenge\\KHZ_UDP_INJECT_COPY' in data and sender == ('127.0.0.1', SERVER_PORT), data.decode('latin1'))
        rcon(f'khz_fixture_udp badprotocol 127.0.0.1:{DIRECT_PORT}')
        data, sender = packet(primary)
        check('injected invalid protocol still rejected by actual admission', b'requires cod4x1khz protocol 10002' in data and sender == ('127.0.0.1', SERVER_PORT), data.decode('latin1'))
        rcon(f'khz_fixture_udp badproof 127.0.0.1:{DIRECT_PORT}')
        data, sender = packet(primary)
        check('injected missing identity proof still rejected by actual admission', data.startswith(b'\xff' * 4 + b'error\n') and b'connectResponse' not in data and sender == ('127.0.0.1', SERVER_PORT), data.decode('latin1'))

        report = rcon('khz_fixture_udp report')
        fields = dict((name, int(value)) for name, value in re.findall(r'(\w+)=(-?\d+)', report))
        check('callbacks correct public32 argument layout/native owner and direct paths bypass redundant events',
            all(fields.get(name) == 1 for name in ('rx_drop', 'rx_edit', 'rx_address', 'tx_drop', 'tx_edit', 'tx_address')) and
            fields.get('callback_layout') == 3 and fields.get('callback_owner') == 3 and fields.get('direct_events') == 0 and fields.get('inject_events') == 0, report)
        check('native pinned DNS families/default unsigned ports complete and main owner refuses blocking DNS',
            fields.get('worker_done') == 1 and fields.get('worker_good') == 1 and fields.get('main_dns') == 0 and fields.get('main_output') == 0, report)

        rcon(f'khz_fixture_udp cancel 127.0.0.1:{DIRECT_PORT}')
        data, sender = packet(primary)
        check('disabled module injection generation is rejected before dispatch', not data, {'data': data.hex(), 'sender': sender})
        stats = query(SERVER_PORT, 'rcon local-khz-benchmark khz_json', 'KHZ_JSON', .6)
        last_rcon = time.monotonic()
        network = stats.get('plugins', {}).get('network', {})
        check('native queued injection and DNS counters record actual work/rejection', network.get('injected') == 4 and network.get('injection_delivered') == 3 and network.get('injection_rejected') >= 1 and network.get('dns', {}).get('completed', 0) >= 4, network)
        check('native direct sends and both actual veto paths accounted independently', network.get('udp_sent') == 2 and network.get('udp_send_failed') == 5 and network.get('send_veto') == 1 and network.get('receive_veto') == 1, network)
        rcon('unloadPlugin khz_regression')
        stats = query(SERVER_PORT, 'rcon local-khz-benchmark khz_json', 'KHZ_JSON', .6)
        last_rcon = time.monotonic()
        dns = stats.get('plugins', {}).get('network', {}).get('dns', {})
        check('native module lifetime ends after worker and packet queue retirement', stats.get('plugins', {}).get('loaded') == 0 and dns.get('pending') == 0 and dns.get('retiring') == 0 and stats.get('plugins', {}).get('threads', {}).get('active') == 0, stats.get('plugins', {}))
        if time.monotonic() > deadline:
            raise TimeoutError('Owned native fixture exceeded35-second scope')
    except Exception as error:
        result['error'] = str(error)
    finally:
        for process, stream in reversed(owned):
            if process.poll() is None:
                try:
                    rcon('quit'); process.wait(timeout=1.5)
                except (OSError, subprocess.TimeoutExpired):
                    process.terminate()
                    try:
                        process.wait(timeout=1.5)
                    except subprocess.TimeoutExpired:
                        process.kill(); process.wait(timeout=1.5)
            stream.close()
        for name in ('primary', 'alternate'):
            if name in locals():
                locals()[name].close()
        data = log.read_bytes() if log.exists() else b''
        if prior and data.startswith(prior):
            data = data[len(prior):]
        (output / 'native-server.log').write_bytes(data)
        result['owned_processes_exited'] = bool(owned) and all(process.poll() is not None for process, stream in owned)
        result['elapsed_seconds'] = time.monotonic() - started
        result['maximum_seconds'] = 35
        if result['elapsed_seconds'] > 35:
            result['error'] = result.get('error', 'Owned fixture exceeded35-second time bound including cleanup')
        result['passed'] = not result.get('error') and result['owned_processes_exited'] and bool(result['cases']) and all(case['passed'] for case in result['cases'])
        (output / 'result.json').write_text(json.dumps(result, indent=2))
        print(json.dumps({'passed': result['passed'], 'elapsed_seconds': result['elapsed_seconds'], 'error': result.get('error')}, indent=2))
        print(output / 'result.json')
    return 0 if result['passed'] else 1


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-archive', required=True, help='Project-local coherent build archive; checked before launch')
    args = parser.parse_args()
    raise SystemExit(main(args.build_archive))
