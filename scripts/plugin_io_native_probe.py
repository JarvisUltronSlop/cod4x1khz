"""Short owned deployed-DLL FS/TCP probe; no builds or GUI automation.

Run only after the parent has deployed a coherent SDK/server checkpoint.
Uses isolated dedicated port 28982, no human clients/bots, <=30 seconds.
"""
from __future__ import annotations
import hashlib
import json
from pathlib import Path
import re
import shutil
import socket
import subprocess
import time
import zipfile
import zlib
import struct
from plugin_fs_probe import encrypt
from plugin_tcp_fixtures import Fixture

PROJECT = Path(__file__).resolve().parent.parent
RUNTIME = PROJECT / 'runtime'
PORT = 28982


def make_archive(path):
    entries = [('plain.dat', b'native-zip-data', 0, None),
               ('deflated.dat', b'native-deflate-data', 8, None),
               ('encrypted.dat', b'native-secret-data', 8, b'native-secret')]
    local, central = bytearray(), bytearray()
    for suffix, plain, method, password in entries:
        name = ('khz_fs_fixture/' + suffix).encode('ascii')
        crc = zlib.crc32(plain)
        if method == 8:
            encoder = zlib.compressobj(wbits=-15)
            data = encoder.compress(plain) + encoder.flush()
        else:
            data = plain
        flags = int(password is not None)
        if password:
            data = encrypt(data, password, crc)
        offset = len(local)
        local += struct.pack('<IHHHHHIIIHH', 0x04034b50, 20, flags, method, 0, 0,
                             crc, len(data), len(plain), len(name), 0) + name + data
        central += struct.pack('<IHHHHHHIIIHHHHHII', 0x02014b50, 20, 20, flags, method, 0, 0,
                               crc, len(data), len(plain), len(name), 0, 0, 0, 0, 0, offset) + name
    data = local + central + struct.pack('<IHHHHIIH', 0x06054b50, 0, 0, len(entries), len(entries), len(central), len(local), 0)
    with path.open('xb') as stream:
        stream.write(data)
    with zipfile.ZipFile(path) as archive:
        for suffix, plain, unused, password in entries:
            assert archive.read('khz_fs_fixture/' + suffix, pwd=password) == plain
    return {name: hashlib.sha256(plain).hexdigest() for name, plain, unused, password in entries}


def main():
    stamp = time.strftime('%Y%m%d-%H%M%S') + f'-{time.time_ns() % 1000000:06d}'
    output = PROJECT / 'results' / ('plugin-io-native-' + stamp)
    output.mkdir()
    prefix = 'khz_sdk_fixture_' + stamp.replace('-', '_')
    home = RUNTIME / 'benchmarks' / f'server-{PORT}'
    package = home / 'main' / ('khz_fs_fixture_' + stamp.replace('-', '_') + '.iwd')
    native_log = home / 'main/khz_server.log'
    expected_roots = [home / prefix, home / 'main' / prefix, RUNTIME / prefix]
    result = {'scope': 'Actual rebuilt DLL FS/TCP APIs in an isolated dedicated custom server; no general feature-parity claim',
              'port': PORT, 'cases': [], 'commands': [], 'sha256': {}, 'owned_prefix': prefix}
    process = stdio = ipv4 = ipv6 = None
    owned_package = False
    started = time.monotonic()
    deadline = started + 30
    last_rcon = 0
    before = b''

    def fresh_log():
        data = native_log.read_bytes() if native_log.exists() else b''
        return data[len(before):] if before and data.startswith(before) else data

    def last_marker(prefix):
        lines = [line for line in fresh_log().decode('latin1').splitlines() if prefix in line]
        return lines[-1] if lines else ''

    def rcon(command):
        nonlocal last_rcon
        time.sleep(max(0, 1.08 - (time.monotonic() - last_rcon)))
        chunks = []
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as udp:
            udp.settimeout(.5)
            udp.sendto(b'\xff' * 4 + ('rcon local-khz-benchmark ' + command).encode('ascii'), ('127.0.0.1', PORT))
            last_rcon = time.monotonic()
            try:
                while True:
                    data, sender = udp.recvfrom(65536)
                    if sender != ('127.0.0.1', PORT):
                        continue
                    if data.startswith(b'\xff' * 4):
                        data = data[4:]
                    if data.startswith(b'print\n'):
                        data = data[6:]
                    chunks.append(data.decode('latin1'))
            except (socket.timeout, ConnectionResetError):
                # A not-yet-bound Windows UDP server reports ICMP as reset.
                # Startup retries below; an absent later reply still fails
                # its semantic checks rather than inventing a success.
                pass
        reply = ''.join(chunks)
        result['commands'].append({'command': command, 'reply': reply})
        return reply

    def check(name, okay, evidence):
        result['cases'].append({'name': name, 'passed': bool(okay), 'evidence': evidence})

    try:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as reserve:
            reserve.bind(('127.0.0.1', PORT))
        for path in expected_roots:
            if path.exists():
                raise FileExistsError('Refusing to reuse fixture output ' + str(path))
        package.parent.mkdir(parents=True, exist_ok=True)
        result['archive_entries'] = make_archive(package)
        owned_package = True
        shutil.copyfile(package, output / 'fixture.iwd')
        for name in ['cod4x1khz_server.exe', 'plugins/khz_regression.dll', 'main/zz_cod4x1khz_bots.iwd']:
            path = RUNTIME / name
            result['sha256'][str(path)] = hashlib.sha256(path.read_bytes()).hexdigest()
        for name in ['engine/src/khz/khz_plugins.cpp', 'engine/src/khz/khz_plugin_fs.cpp',
                     'engine/src/khz/khz_plugin_fs_adapter.cpp', 'engine/src/khz/khz_plugin_unzip.c',
                     'engine/src/khz/khz_plugin_files.inl', 'engine/src/khz/khz_plugin_tcp.cpp',
                     'engine/src/khz/khz_plugin_tcp.inl', 'sdk/include/public_file_api.h',
                     'sdk/include/public_tcp_api.h', 'sdk/tests/regression_plugin.c',
                     'sdk/tests/native_file_probe.inc', 'sdk/tests/native_tcp_probe.inc',
                     'engine/src/universal/com_files.cpp', 'engine/src/qcommon/files.cpp',
                     'scripts/plugin_io_native_probe.py']:
            path = PROJECT / name
            result['sha256'][str(path)] = hashlib.sha256(path.read_bytes()).hexdigest()
        ipv4 = Fixture(socket.AF_INET)
        ipv6 = Fixture(socket.AF_INET6)
        before = native_log.read_bytes() if native_log.exists() else b''
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
        while time.monotonic() < deadline - 17:
            if process.poll() is not None:
                raise RuntimeError('Owned server exited before fixture readiness')
            reply = rcon('khz_fixture_report')
            ready = reply if 'KHZ_PLUGIN_FIXTURE frames=' in reply else last_marker('KHZ_PLUGIN_FIXTURE frames=')
            if 'KHZ_PLUGIN_FIXTURE frames=' in ready:
                break
        else:
            raise TimeoutError('Owned server not ready within 13-second startup scope')
        rcon('khz_fixture_files_start ' + prefix)
        rcon(f'khz_fixture_tcp_start "localhost:{ipv4.server_address[1]}" "[::1]:{ipv6.server_address[1]}"')
        file_report = tcp_report = ''
        while time.monotonic() < deadline - 3:
            if 'finished=1' not in file_report:
                reply = rcon('khz_fixture_files_report')
                file_report = reply if 'KHZ_PLUGIN_FILE_FIXTURE ' in reply else last_marker('KHZ_PLUGIN_FILE_FIXTURE ')
            if 'finished=1' not in tcp_report:
                reply = rcon('khz_fixture_tcp_report')
                tcp_report = reply if 'KHZ_PLUGIN_TCP_FIXTURE ' in reply else last_marker('KHZ_PLUGIN_TCP_FIXTURE ')
            if 'finished=1' in file_report and 'finished=1' in tcp_report:
                break
        for label, text in [('filesystem', file_report), ('tcp', tcp_report)]:
            fields = {key: int(value) for key, value in re.findall(r'(\w+)=(\d+)', text)}
            check(label + ' actual pinned job/native completion', fields.get('active') == 0 and fields.get('finished') == 1 and
                  fields.get('cases') == (28 if label == 'filesystem' else 24) and fields.get('failures') == 0 and
                  fields.get('completions') == 1 and fields.get('owner') == 1, {'reply': text, 'fields': fields})
            check(label + ' owner/tick blocking rejection', fields.get('main_rejected') == 1 and fields.get('tick_rejected') == 1, fields)
        check('filesystem retained returned bytes freed on owner', 'retained_free=1' in file_report, file_report)
        before_unload = rcon('khz_json')
        result['stats_before_unload'] = before_unload
        rcon('unloadPlugin khz_regression')
        after_unload = rcon('khz_json')
        result['stats_after_unload'] = after_unload
        match = re.search(r'KHZ_JSON\s+(\{[^\r\n]*\})', after_unload)
        stats = json.loads(match.group(1)) if match else {}
        plugins = stats.get('plugins', {})
        check('completed plugin unloaded and private storage drained', plugins.get('loaded') == 0 and
              plugins.get('filesystem', {}).get('streams') == 0 and plugins.get('filesystem', {}).get('buffers') == 0 and
              plugins.get('tcp', {}).get('connected') == 0 and plugins.get('tcp', {}).get('pending') == 0 and
              plugins.get('tcp', {}).get('retiring') == 0, plugins)
        fresh = fresh_log()
        (output / 'native-server.log').write_bytes(fresh)
        check('dynamic GSC fixture remains functional', b'KHZ_PLUGIN_SCRIPT_FIXTURE dynamic function and usercall passed' in fresh, fresh.decode('latin1')[-1800:])
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
        for listener in [ipv4, ipv6]:
            if listener:
                listener.close()
        result['tcp_events'] = {'ipv4': ipv4.events if ipv4 else [], 'ipv6': ipv6.events if ipv6 else []}
        result['written_files'] = {}
        for path in expected_roots:
            if path.exists():
                absolute = path.resolve(strict=True)
                if absolute.parent not in [home.resolve(), (home / 'main').resolve(), RUNTIME.resolve()] or absolute.name != prefix:
                    result['cleanup_error'] = 'Refusing unexpected cleanup target ' + str(absolute)
                    continue
                for item in path.rglob('*'):
                    if item.is_file():
                        result['written_files'][str(item)] = {'bytes': item.stat().st_size, 'sha256': hashlib.sha256(item.read_bytes()).hexdigest()}
                shutil.copytree(path, output / ('written-' + str(expected_roots.index(path))))
                shutil.rmtree(path)
        if owned_package:
            package.unlink()
        result['elapsed_seconds'] = time.monotonic() - started
        result['passed'] = not result.get('error') and not result.get('cleanup_error') and result.get('owned_process_exited') and bool(result['cases']) and all(case['passed'] for case in result['cases'])
        (output / 'result.json').write_text(json.dumps(result, indent=2))
        print(json.dumps({'passed': result['passed'], 'elapsed_seconds': result['elapsed_seconds'], 'error': result.get('error')}, indent=2))
        print(output / 'result.json')
    return 0 if result['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
