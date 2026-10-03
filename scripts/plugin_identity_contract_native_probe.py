"""Archive-bound actual identity classifier/GUID exports, dedicated only.

Reuse the existing twenty-command fixture. Maximum30s includes normal cleanup;
never build/deploy, change an existing profile, or publish capability evidence.
Run only after the build owner grants an exclusive native fixture window.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import socket
import stat
import subprocess
import time

from plugin_fixture_archive import bind_archive

PROJECT = Path(__file__).resolve().parents[1]
RUNTIME = PROJECT / 'runtime'
CLASSIFIER_FIELDS = {'cases': 32838, 'failures': 0, 'matrix': 32768,
                     'bit_flips': 64, 'literals': 6, 'provider_authentication': 0}
GUID_FIELDS = {'cases': 247, 'failures': 0, 'goldens': 7, 'case_variants': 3,
               'invalid_hex': 160, 'wrong_lengths': 64, 'null': 1,
               'terminated_prefix': 1, 'provider_authentication': 0,
               'provider_failure_injected': 0}


def marker(text, name):
    """Require one canonical marker; duplicate/error output cannot pass."""
    lines = [line[line.index(name):] for line in text.splitlines() if name in line]
    if len(lines) != 1:
        raise ValueError(f'{name}: expected one native marker, observed {len(lines)}')
    line = lines[0]
    if not re.fullmatch(re.escape(name) + r'(?: [a-z_]+=\d+)+', line):
        raise ValueError(f'{name}: noncanonical native marker')
    fields = re.findall(r'([a-z_]+)=(\d+)', line)
    if len({key for key, value in fields}) != len(fields):
        raise ValueError(f'{name}: duplicate field')
    return {'text': line, 'fields': {key: int(value) for key, value in fields}}


def classified(text):
    if 'KHZ_IDENTITY_CONTRACT_FAILURE' in text:
        raise ValueError('Actual classifier exported a failure detail')
    values = {name: marker(text, name) for name in
              ('KHZ_IDENTITY_INDIVIDUAL', 'KHZ_IDENTITY_STEAM_ACCOUNT')}
    if any(value['fields'] != CLASSIFIER_FIELDS for value in values.values()):
        raise ValueError('Actual classifier case counts or failure/provider fields differ')
    return values


def converted_guid(text):
    if 'KHZ_IDENTITY_CONTRACT_FAILURE' in text:
        raise ValueError('Actual GUID exported a failure detail')
    value = marker(text, 'KHZ_IDENTITY_GUID')
    if value['fields'] != GUID_FIELDS:
        raise ValueError('Actual GUID case counts or failure/provider fields differ')
    return value


def rejected(text):
    expected = 'KHZ_IDENTITY_DISPATCH rejected=1 expected_subcommand=format|classify|guid'
    return text.count(expected) == 1 and not any(value in text for value in (
        'KHZ_IDENTITY_INDIVIDUAL', 'KHZ_IDENTITY_STEAM_ACCOUNT', 'KHZ_IDENTITY_GUID',
        'KHZ_IDENTITY_FORMAT_', 'KHZ_PLUGIN_IDENTITY_FIXTURE'))


def project_path(name, *, file=False, directory=False):
    """Enroll an ordinary project path before resolving or opening it.

    Reject lexical parent traversal and every existing reparse ancestor. An
    escaped runtime junction must never become an authorized fixture root.
    """
    supplied = Path(name)
    if '..' in supplied.parts:
        raise ValueError('Fixture path contains lexical parent traversal')
    path = Path(os.path.abspath(supplied if supplied.is_absolute() else PROJECT / supplied))
    if not path.is_relative_to(PROJECT):
        raise ValueError('Fixture input must remain beneath project')
    for ancestor in (path, *path.parents):
        try:
            attributes = ancestor.lstat()
        except FileNotFoundError:
            continue
        if (stat.S_ISLNK(attributes.st_mode) or
            getattr(attributes, 'st_file_attributes', 0) & getattr(stat, 'FILE_ATTRIBUTE_REPARSE_POINT', 0x400)):
            raise ValueError('Fixture path has a reparse ancestor: ' + str(ancestor))
    resolved = path.resolve()
    if not resolved.is_relative_to(PROJECT):
        raise ValueError('Resolved fixture path escaped project')
    if file and not resolved.is_file():
        raise ValueError('Fixture input is not an ordinary existing file')
    if directory and not resolved.is_dir():
        raise ValueError('Fixture input is not an ordinary existing directory')
    return resolved


def project_file(name):
    return project_path(name, file=True)


def fresh_home(port):
    if type(port) is not int or not 1024 <= port <= 65535:
        raise ValueError('Fixture home requires a bounded actual UDP port')
    runtime = project_path(RUNTIME, directory=True)
    benchmarks = project_path(runtime / 'benchmarks')
    if benchmarks.exists() and not benchmarks.is_dir():
        raise ValueError('Fixture benchmark parent is not an ordinary directory')
    home = project_path(benchmarks / f'server-{port}')
    if home.parent != benchmarks or benchmarks.parent != runtime:
        raise ValueError('Fixture home must be a direct ordinary runtime/benchmarks child')
    if home.exists():
        raise ValueError('Fixture benchmark home must be previously absent')
    return home


def check_goldens(dotnet, oracle):
    records = dotnet.get('records')
    expected = oracle.get('records')
    if (dotnet.get('passed') is not True or dotnet.get('vectors') != 7 or
        not isinstance(records, list) or len(records) != 7 or
        not isinstance(expected, list) or len(expected) != 7 or
        any(not isinstance(row, dict) or row.get('passed') is not True for row in records)):
        raise ValueError('Independent .NET seven-vector comparison failed/missing')
    if (any(not isinstance(row.get('guid'), str) or not re.fullmatch(r'[0-9a-f]{32}', row['guid']) or
            type(row.get('actual')) is not int or not 0 <= row['actual'] <= 0xffffffffffffffff
            for row in records) or
        any(not isinstance(row, dict) or not isinstance(row.get('guid'), str) or
            not re.fullmatch(r'[0-9a-f]{32}', row['guid']) or type(row.get('expected')) is not int
            for row in expected) or len({row['guid'] for row in records}) != 7 or
        [(row.get('guid'), row.get('actual')) for row in records] !=
        [(row.get('guid'), row.get('expected')) for row in expected]):
        raise ValueError('Independent Python/.NET seven distinct values disagree')


def bind_oracle_review(name, result):
    path = project_file(name)
    review = json.loads(path.read_text(encoding='utf-8-sig'))
    if (review.get('passed') is not True or review.get('preparation_only') is not True or
        review.get('native_sdk_execution') is not False or review.get('semantic_proof') is not False or
        review.get('classifier_expected_cases_per_export') != 32838 or
        review.get('guid_vectors') != 7 or review.get('guid_expected_native_cases') != 247):
        raise ValueError('Independent offline oracle review is missing or has wrong scope/counts')
    result['sha256'][str(path)] = hashlib.sha256(path.read_bytes()).hexdigest()
    for relative, expected in review.get('source_sha256', {}).items():
        source = project_file(relative)
        actual = hashlib.sha256(source.read_bytes()).hexdigest()
        if expected != actual:
            raise ValueError('Independent oracle review source changed: ' + relative)
        result['sha256'][str(source)] = actual
    for relative, expected in review.get('log_sha256', {}).items():
        log = project_file(path.parent / relative)
        if not log.is_relative_to(path.parent):
            raise ValueError('Independent review log escaped its artifact directory')
        actual = hashlib.sha256(log.read_bytes()).hexdigest()
        if expected != actual:
            raise ValueError('Independent oracle review log changed: ' + relative)
        result['sha256'][str(log)] = actual
    dotnet_path = project_file(path.parent / 'dotnet-goldens.json')
    oracle_path = project_file('config/staged/identity_guid_goldens.json')
    if 'dotnet-goldens.json' not in review.get('log_sha256', {}):
        raise ValueError('Independent review must bind the exact .NET records file')
    dotnet = json.loads(dotnet_path.read_text(encoding='utf-8-sig'))
    oracle = json.loads(oracle_path.read_text())
    check_goldens(dotnet, oracle)
    required = {'sdk/tests/native_identity_contract_probe.inc', 'sdk/tests/identity_guid_goldens.inc',
                'engine/src/khz/khz_identity.cpp', 'upstream/cod4x_server/src/sapi.c',
                'config/staged/identity_guid_goldens.json'}
    if not required.issubset(review.get('source_sha256', {})):
        raise ValueError('Independent oracle review lacks required native/data/source bindings')
    result['oracle_review'] = {'path': str(path), 'passed': True,
                              'vectors': 7, 'native_execution': False}


def main(build_archive, oracle_review, maximum_seconds=30, first_port=28986):
    output = project_path(PROJECT / 'results' / ('plugin-identity-contract-native-' +
        time.strftime('%Y%m%d-%H%M%S') + f'-{time.time_ns() % 1000000:06d}'))
    output.mkdir()
    result = {'scope': 'Actual rebuilt C DLL classifier bit matrix and independent GUID goldens/bounds; parser unchanged; no authentication',
              'cases': [], 'commands': [], 'processes': [], 'sha256': {},
              'public_proof_published': False, 'capability_observations': {}}
    started = time.monotonic()
    deadline = started + maximum_seconds
    last_rcon = 0.0
    port = None
    home = None
    process = stream = reservation = None
    cleanup = {'requested_quit': False, 'normal_exit': False, 'forced': False}

    def check(name, passed, evidence):
        result['cases'].append({'name': name, 'passed': bool(passed), 'evidence': evidence})

    def rcon(command, final=False):
        nonlocal last_rcon
        wait = max(0, 1.08 - (time.monotonic() - last_rcon))
        if time.monotonic() + wait + .55 > deadline - (1.5 if final else 5):
            raise TimeoutError('Insufficient bounded command/normal-cleanup budget')
        if wait:
            time.sleep(wait)
        chunks, byte_count = [], 0
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as udp:
            udp.sendto(b'\xff' * 4 + ('rcon local-khz-identity-contract ' + command).encode('ascii'),
                       ('127.0.0.1', port))
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
                if sender != ('127.0.0.1', port):
                    continue
                if packet.startswith(b'\xff' * 4):
                    packet = packet[4:]
                if packet.startswith(b'print\n'):
                    packet = packet[6:]
                byte_count += len(packet)
                if byte_count > 65536:
                    raise RuntimeError('Native redirect exceeded bounded64KiB')
                chunks.append(packet.decode('latin1'))
        reply = ''.join(chunks)
        result['commands'].append({'port': port, 'command': command, 'reply': reply,
            'reply_window_seconds': .55, 'received_bytes': byte_count, 'no_reply': not chunks})
        return reply

    def stop_owned():
        if process is None:
            return
        if process.poll() is None:
            try:
                cleanup['requested_quit'] = True
                rcon('quit', final=True)
                process.wait(timeout=min(1.5, max(.01, deadline - time.monotonic())))
                cleanup['normal_exit'] = process.returncode == 0
            except (OSError, TimeoutError, subprocess.TimeoutExpired) as error:
                cleanup['error'] = str(error)
                cleanup['forced'] = True
                process.terminate()
                try:
                    process.wait(timeout=.5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=.5)
        cleanup['owned_process_exited'] = process.poll() is not None
        cleanup['exit_code'] = process.returncode

    try:
        compiled = ('engine/src/khz/khz_plugins.cpp', 'engine/src/khz/khz_identity.cpp',
            'engine/src/khz/khz_identity.h', 'engine/src/khz/khz_admin_server.cpp',
            'engine/src/khz/khz_controls.cpp', 'engine/src/khz/khz_controls.h',
            'engine/src/khz/khz_paths.cpp', 'native/steam_identity_format.hpp',
            'tests/identity_admin_tests.cpp', 'sdk/include/pinc.h',
            'sdk/tests/regression_plugin.c', 'sdk/tests/native_stats_probe.inc',
            'sdk/tests/native_identity_format_probe.inc', 'sdk/tests/native_identity_contract_probe.inc',
            'sdk/tests/identity_guid_goldens.inc')
        runtime = project_path(RUNTIME, directory=True)
        archive = project_file(build_archive)
        for relative in (*compiled, 'runtime/cod4x1khz_server.exe',
                         'runtime/plugins/khz_regression.dll', 'runtime/main/zz_cod4x1khz_bots.iwd',
                         'native/plugin_registry.hpp', 'upstream/cod4x_server/src/plugin_handler.h'):
            project_file(relative)
        build = bind_archive(PROJECT, str(archive), result,
            ('runtime/cod4x1khz_server.exe', 'runtime/plugins/khz_regression.dll'), compiled)
        harness = Path(__file__).resolve()
        result['sha256'][str(harness)] = hashlib.sha256(harness.read_bytes()).hexdigest()
        bind_oracle_review(oracle_review, result)
        check('current source/DLL/server/package and independent oracle bindings match', True,
              {'build': build, 'archive': result['build_archive'], 'oracle': result['oracle_review']})
        for candidate in range(first_port, min(first_port + 64, 65536)):
            candidate_home = project_path(runtime / 'benchmarks' / f'server-{candidate}')
            if candidate_home.exists():
                continue
            candidate_home = fresh_home(candidate)
            reserve = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            try:
                reserve.bind(('127.0.0.1', candidate))
            except OSError:
                reserve.close()
                continue
            port, home, reservation = candidate, candidate_home, reserve
            break
        if home is None:
            raise RuntimeError('No free bounded port with a previously absent project benchmark home')
        result['home'] = {'path': str(home), 'absent_before_launch': not home.exists(),
                          'format_dvar_override': None}
        arguments = ['+set', 'dedicated', '1', '+set', 'net_ip', '127.0.0.1',
            '+set', 'net_port', str(port), '+set', 'khz_benchmark', '1', '+set', 'khz_test_controls', '1',
            '+set', 'logfile', '2', '+set', 'rcon_password', 'local-khz-identity-contract',
            '+set', 'sv_maxclients', '1', '+set', 'g_gametype', 'dm',
            '+set', 'khz_bots', '0', '+set', 'khz_bot_count', '0',
            '+loadPlugin', 'khz_regression', '+set', 'khz_plugin_tests', '1', '+map', 'mp_killhouse']
        stream = (output / 'server-stdio.log').open('wb')
        if fresh_home(port) != home:
            raise RuntimeError('Previously absent fixture home appeared before launch')
        reservation.close()
        process = subprocess.Popen([str(project_file(runtime / 'cod4x1khz_server.exe')), *arguments], cwd=runtime,
            stdout=stream, stderr=subprocess.STDOUT, creationflags=subprocess.CREATE_NO_WINDOW)
        result['processes'].append({'pid': process.pid, 'port': port, 'arguments': arguments})
        (output / 'processes.json').write_text(json.dumps(result['processes'], indent=2))
        while time.monotonic() < min(started + 8, deadline - 15):
            if process.poll() is not None:
                raise RuntimeError('Owned server exited before loaded-DLL readiness')
            reply = rcon('khz_fixture_report')
            if 'KHZ_PLUGIN_FIXTURE frames=' in reply:
                result['readiness_reply'] = reply
                break
        else:
            raise TimeoutError('Loaded identity fixture did not become ready')

        formats = rcon('khz_fixture_identity format')
        selected = marker(formats, 'KHZ_IDENTITY_FORMAT_SELECTED')
        explicit64 = marker(formats, 'KHZ_IDENTITY_FORMAT_EXPLICIT64')
        steam3 = marker(formats, 'KHZ_IDENTITY_FORMAT_STEAM3')
        check('fresh default decimal mode and exact selected-format setting restoration',
              selected['fields'] == {'cases': 99, 'failures': 0, 'initial': 1, 'restored': 1, 'provider_authentication': 0}, selected)
        check('existing explicit64/Steam3 subcommand remains functional',
              explicit64['fields'] == {'cases': 96, 'failures': 0, 'provider_authentication': 0} and
              steam3['fields'] == {'cases': 128, 'failures': 0, 'provider_authentication': 0} and
              'KHZ_IDENTITY_FORMAT_FAILURE' not in formats, {'explicit64': explicit64, 'steam3': steam3})
        result['default_markers'] = {'selected': selected, 'explicit64': explicit64, 'steam3': steam3}

        classifiers = classified(rcon('khz_fixture_identity classify'))
        for export, name in (('Plugin_SteamIDIsIndividual', 'KHZ_IDENTITY_INDIVIDUAL'),
                             ('Plugin_SteamIDIsIndividualAndSteamAccount', 'KHZ_IDENTITY_STEAM_ACCOUNT')):
            check('actual ' + export + ' bit-field matrix/literals', True, classifiers[name])
            result['capability_observations'][export] = {'actual_cases': 32838, 'actual_failures': 0,
                'complete_contract_candidate': True, 'reviewed_or_published_proven': False,
                'marker': classifiers[name]}
        guid = converted_guid(rcon('khz_fixture_identity guid'))
        check('actual GUID independent goldens, case, invalid input and read-only boundaries', True, guid)
        result['capability_observations']['Plugin_GUID2PlayerID'] = {'actual_cases': 247,
            'actual_failures': 0, 'complete_contract_candidate': False,
            'reviewed_or_published_proven': False, 'marker': guid,
            'pending_semantics': ['Crypto-provider failure injection was not performed; complete contract needs review.']}
        for command in ('khz_fixture_identity unknown', 'khz_fixture_identity classify extra'):
            invalid = rcon(command)
            check('explicit bounded dispatcher refusal: ' + command, rejected(invalid), invalid)
        old = rcon('khz_fixture_identity')
        check('existing no-argument conversion oracle is preserved',
              'KHZ_PLUGIN_IDENTITY_FIXTURE conversions=1 guid_golden=2310346614158149229 official_provider=unavailable' in old, old)
        rcon('unloadPlugin khz_regression')
        statistics = rcon('khz_json')
        found = re.search(r'KHZ_JSON\s+(\{[^\r\n]*\})', statistics)
        state = json.loads(found.group(1)) if found else {}
        check('actual fixture module unloads before normal owned quit',
              state.get('plugins', {}).get('loaded') == 0, state.get('plugins', {}))
        result['after_unload'] = state
        if process.poll() is not None:
            raise RuntimeError('Owned server exited before explicit normal cleanup')
    except Exception as error:
        result['error'] = str(error)
    finally:
        if reservation is not None:
            reservation.close()
        stop_owned()
        if stream is not None:
            stream.close()
        result['cleanup'] = cleanup
        result['owned_processes_exited'] = process is not None and process.poll() is not None
        result['normal_cleanup'] = cleanup['normal_exit'] and not cleanup['forced']
        result['elapsed_seconds'] = time.monotonic() - started
        result['maximum_seconds'] = maximum_seconds
        if home is not None:
            log = home / 'main' / 'khz_server.log'
            if log.exists():
                try:
                    (output / 'native-server.log').write_bytes(project_file(log).read_bytes())
                except (OSError, ValueError) as error:
                    result['error'] = result.get('error', 'Native log confinement failed: ' + str(error))
        for name in ('server-stdio.log', 'native-server.log', 'processes.json'):
            artifact = output / name
            if artifact.exists():
                result['sha256'][str(artifact)] = hashlib.sha256(artifact.read_bytes()).hexdigest()
        if result['elapsed_seconds'] > maximum_seconds:
            result['error'] = result.get('error', 'Fixture exceeded its total bound including cleanup')
        result['passed'] = (not result.get('error') and result['normal_cleanup'] and
            bool(result['cases']) and all(case['passed'] for case in result['cases']))
        (output / 'result.json').write_text(json.dumps(result, indent=2))
        print(json.dumps({'passed': result['passed'], 'elapsed_seconds': result['elapsed_seconds'],
                          'normal_cleanup': result['normal_cleanup'], 'error': result.get('error')}, indent=2))
        print(output / 'result.json')
    return 0 if result['passed'] else 1


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-archive', required=True)
    parser.add_argument('--oracle-review', required=True)
    parser.add_argument('--seconds', type=float, default=30)
    parser.add_argument('--first-port', type=int, default=28986)
    arguments = parser.parse_args()
    if not 20 <= arguments.seconds <= 30:
        parser.error('--seconds must be20..30')
    if not 1024 <= arguments.first_port <= 65472:
        parser.error('--first-port must be1024..65472')
    raise SystemExit(main(arguments.build_archive, arguments.oracle_review, arguments.seconds, arguments.first_port))
