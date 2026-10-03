"""Archive-bound loaded-DLL Steam format/default/configured-zero fixture.

Two serial dedicated-only, zero-bot servers use previously absent benchmark
homes beneath this project. This never builds, deploys or edits an existing
profile. Maximum35s includes exact owned-process cleanup. Run only when the
build owner authorizes the exclusive native fixture sequence.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import socket
import subprocess
import time

from plugin_fixture_archive import bind_archive

PROJECT = Path(__file__).resolve().parents[1]
RUNTIME = PROJECT / 'runtime'


def main(build_archive, maximum_seconds=30, first_port=28986):
    output = PROJECT / 'results' / ('plugin-identity-format-native-' + time.strftime('%Y%m%d-%H%M%S') + f'-{time.time_ns() % 1000000:06d}')
    output.mkdir()
    result = {'scope': 'Actual rebuilt C DLL selected Steam3/Steam64 formatter, explicit64 bounds, fresh default and configured0 preservation; no provider authentication',
              'cases': [], 'commands': [], 'processes': [], 'phases': [], 'sha256': {}}
    owned = []
    started = time.monotonic()
    deadline = started + maximum_seconds
    last_rcon = 0.0
    phase_port = None
    next_port = first_port

    def check(name, passed, evidence):
        result['cases'].append({'name': name, 'passed': bool(passed), 'evidence': evidence})

    def rcon(text, cleanup=False):
        nonlocal last_rcon
        wait = max(0.0, 1.08 - (time.monotonic() - last_rcon))
        if time.monotonic() + wait + .55 > deadline - (2 if cleanup else 3):
            raise TimeoutError('Owned fixture has insufficient command/cleanup budget')
        if wait:
            time.sleep(wait)
        chunks = []
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as udp:
            udp.sendto(b'\xff' * 4 + ('rcon local-khz-identity-format ' + text).encode('ascii'), ('127.0.0.1', phase_port))
            last_rcon = time.monotonic()
            receive_deadline = last_rcon + .55
            while time.monotonic() < receive_deadline:
                remaining = receive_deadline - time.monotonic()
                if remaining <= 0:
                    break
                udp.settimeout(min(.2, remaining))
                try:
                    packet, sender = udp.recvfrom(65536)
                except socket.timeout:
                    continue
                except ConnectionResetError:
                    break
                if sender != ('127.0.0.1', phase_port):
                    continue
                if packet.startswith(b'\xff' * 4):
                    packet = packet[4:]
                if packet.startswith(b'print\n'):
                    packet = packet[6:]
                chunks.append(packet.decode('latin1'))
                if sum(map(len, chunks)) > 65536:
                    raise RuntimeError('Bounded native RCON reply exceeded64KiB')
        reply = ''.join(chunks)
        result['commands'].append({'port': phase_port, 'command': text, 'reply': reply})
        return reply

    def markers(text):
        parsed = {}
        for marker in ('KHZ_IDENTITY_FORMAT_SELECTED', 'KHZ_IDENTITY_FORMAT_EXPLICIT64', 'KHZ_IDENTITY_FORMAT_STEAM3'):
            lines = [line[line.index(marker):] for line in text.splitlines() if marker in line]
            line = lines[-1] if lines else ''
            parsed[marker] = {'text': line, 'fields': dict((key, int(value)) for key, value in re.findall(r'([a-z_]+)=(\d+)', line))}
        return parsed

    def choose_fresh_home():
        nonlocal next_port
        for port in range(next_port, min(next_port + 64, 65536)):
            home = RUNTIME / 'benchmarks' / f'server-{port}'
            if not home.resolve().is_relative_to(RUNTIME.resolve()):
                raise ValueError('Benchmark home must remain beneath project runtime')
            if home.exists():
                continue
            reservation = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            try:
                reservation.bind(('127.0.0.1', port))
            except OSError:
                reservation.close()
                continue
            next_port = port + 1
            return port, home, reservation
        raise RuntimeError('No free port with a previously absent benchmark home in the bounded range')

    def stop(process, stream):
        try:
            if process.poll() is None:
                try:
                    rcon('quit', cleanup=True)
                    process.wait(timeout=.6)
                except (OSError, TimeoutError, subprocess.TimeoutExpired):
                    process.terminate()
                    try:
                        process.wait(timeout=.6)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait(timeout=.6)
        finally:
            stream.close()

    try:
        binaries = ('runtime/cod4x1khz_server.exe', 'runtime/plugins/khz_regression.dll')
        compiled = ('engine/src/khz/khz_plugins.cpp', 'engine/src/khz/khz_admin_server.cpp',
                    'engine/src/khz/khz_controls.cpp', 'engine/src/khz/khz_controls.h',
                    'engine/src/khz/khz_identity.cpp', 'engine/src/khz/khz_identity.h',
                    'engine/src/khz/khz_paths.cpp', 'native/steam_identity_format.hpp',
                    'tests/steam_identity_format_tests.cpp', 'sdk/include/pinc.h',
                    'sdk/tests/regression_plugin.c', 'sdk/tests/native_stats_probe.inc',
                    'sdk/tests/native_identity_format_probe.inc')
        build = bind_archive(PROJECT, build_archive, result, binaries, compiled)
        harness = Path(__file__).resolve()
        result['sha256'][str(harness)] = hashlib.sha256(harness.read_bytes()).hexdigest()
        check('deployed server/DLL and reviewed formatter/fixture sources match selected coherent archive',
              True, {'build': build, 'archive': result['build_archive']})
        for name, configured, expected in (('fresh-default', None, 1), ('configured-zero', '0', 0)):
            phase_port, home, reservation = choose_fresh_home()
            phase = {'name': name, 'port': phase_port, 'home': str(home), 'home_absent_before_launch': not home.exists(),
                     'configured': configured, 'expected_initial': expected}
            result['phases'].append(phase)
            arguments = ['+set', 'dedicated', '1', '+set', 'net_ip', '127.0.0.1', '+set', 'net_port', str(phase_port),
                         '+set', 'khz_benchmark', '1', '+set', 'khz_test_controls', '1',
                         '+set', 'logfile', '2', '+set', 'rcon_password', 'local-khz-identity-format',
                         '+set', 'sv_maxclients', '1', '+set', 'g_gametype', 'dm',
                         '+set', 'khz_bots', '0', '+set', 'khz_bot_count', '0']
            if configured is not None:
                arguments += ['+set', 'sv_usesteam64id', configured]
            arguments += ['+loadPlugin', 'khz_regression', '+set', 'khz_plugin_tests', '1', '+map', 'mp_killhouse']
            stream = (output / (name + '-stdio.log')).open('wb')
            process = None
            try:
                if home.exists():
                    raise RuntimeError('Fresh benchmark home appeared before owned launch')
                reservation.close()
                process = subprocess.Popen([str(RUNTIME / 'cod4x1khz_server.exe'), *arguments], cwd=RUNTIME,
                    stdout=stream, stderr=subprocess.STDOUT, creationflags=subprocess.CREATE_NO_WINDOW)
                owned.append((process, stream, phase_port))
                phase['pid'] = process.pid
                result['processes'].append({'pid': process.pid, 'name': name, 'port': phase_port, 'arguments': arguments})
                (output / 'processes.json').write_text(json.dumps(result['processes'], indent=2))
                ready_deadline = min(time.monotonic() + 8, deadline - (12 if configured is None else 5))
                parsed, reply = {}, ''
                while time.monotonic() < ready_deadline:
                    if process.poll() is not None:
                        raise RuntimeError('Owned server exited before native formatter readiness')
                    reply = rcon('khz_fixture_identity format')
                    parsed = markers(reply)
                    if all(value['text'] for value in parsed.values()):
                        break
                else:
                    raise TimeoutError('Actual formatter fixture did not become ready within bounded phase')
                phase['markers'] = parsed
                selected = parsed['KHZ_IDENTITY_FORMAT_SELECTED']['fields']
                explicit64 = parsed['KHZ_IDENTITY_FORMAT_EXPLICIT64']['fields']
                steam3 = parsed['KHZ_IDENTITY_FORMAT_STEAM3']['fields']
                check(name + ': fresh bootstrap/configured value survives native dvar registration',
                      phase['home_absent_before_launch'] and selected.get('initial') == expected, phase)
                check(name + ': actual selected formatter modes/bounds and original setting restoration',
                      selected.get('cases') == 99 and selected.get('failures') == 0 and selected.get('restored') == 1 and
                      selected.get('provider_authentication') == 0, parsed['KHZ_IDENTITY_FORMAT_SELECTED'])
                check(name + ': actual explicit64 ignores configured mode and preserves rejection guards',
                      explicit64.get('cases') == 96 and explicit64.get('failures') == 0 and
                      explicit64.get('provider_authentication') == 0, parsed['KHZ_IDENTITY_FORMAT_EXPLICIT64'])
                check(name + ': all allowed universes/account types and invalid classifications',
                      steam3.get('cases') == 128 and steam3.get('failures') == 0 and
                      steam3.get('provider_authentication') == 0 and 'KHZ_IDENTITY_FORMAT_FAILURE' not in reply,
                      parsed['KHZ_IDENTITY_FORMAT_STEAM3'])
                conversion = rcon('khz_fixture_identity')
                check(name + ': existing native parser/GUID/classification oracle follows restored mode',
                      'KHZ_PLUGIN_IDENTITY_FIXTURE conversions=1 guid_golden=2310346614158149229 official_provider=unavailable' in conversion,
                      conversion)
                rcon('unloadPlugin khz_regression')
                statistics = rcon('khz_json')
                match = re.search(r'KHZ_JSON\s+(\{[^\r\n]*\})', statistics)
                state = json.loads(match.group(1)) if match else {}
                check(name + ': fixture unloads after real callbacks', state.get('plugins', {}).get('loaded') == 0,
                      state.get('plugins', {}))
            finally:
                reservation.close()
                if process is not None:
                    stop(process, stream)
                    phase['owned_process_exited'] = process.poll() is not None
                else:
                    stream.close()
                log = home / 'main' / 'khz_server.log'
                if log.exists():
                    (output / (name + '-native-server.log')).write_bytes(log.read_bytes())
            if time.monotonic() >= deadline - 3:
                raise TimeoutError('Owned formatter sequence exhausted its cleanup budget')
    except Exception as error:
        result['error'] = str(error)
    finally:
        for process, stream, port in reversed(owned):
            phase_port = port
            if process.poll() is None:
                stop(process, stream)
            elif not stream.closed:
                stream.close()
        result['owned_processes_exited'] = len(owned) == 2 and all(process.poll() is not None for process, stream, port in owned)
        result['elapsed_seconds'] = time.monotonic() - started
        result['maximum_seconds'] = maximum_seconds
        if result['elapsed_seconds'] > maximum_seconds:
            result['error'] = result.get('error', 'Owned native formatter sequence exceeded requested bound including cleanup')
        result['passed'] = not result.get('error') and result['owned_processes_exited'] and bool(result['cases']) and all(case['passed'] for case in result['cases'])
        (output / 'result.json').write_text(json.dumps(result, indent=2))
        print(json.dumps({'passed': result['passed'], 'elapsed_seconds': result['elapsed_seconds'], 'error': result.get('error')}, indent=2))
        print(output / 'result.json')
    return 0 if result['passed'] else 1


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-archive', required=True)
    parser.add_argument('--seconds', type=float, default=30)
    parser.add_argument('--first-port', type=int, default=28986)
    args = parser.parse_args()
    if not 20 <= args.seconds <= 35:
        parser.error('--seconds must be20..35')
    if not 1024 <= args.first_port <= 65472:
        parser.error('--first-port must be1024..65472')
    raise SystemExit(main(args.build_archive, args.seconds, args.first_port))
