"""Run short, owned native SDK checks against deployed binaries, never build them."""
import hashlib
import http.server
import json
from pathlib import Path
import re
import shutil
import socket
import subprocess
import threading
import time
from monitor import query

PROJECT = Path(__file__).resolve().parent.parent
RUNTIME = PROJECT / 'runtime'
SERVER_PORT, CLIENT_PORT = 28982, 28983
OUTPUT = PROJECT / 'results' / ('plugin-native-56-' + time.strftime('%Y%m%d-%H%M%S'))
OUTPUT.mkdir()
result = {'scope': 'Native deployed SDK/script/stat/thread/HTTP integration; private custom pair only',
          'ports': [SERVER_PORT, CLIENT_PORT], 'cases': [], 'commands': [], 'processes': [],
          'sha256': {}, 'http_requests': []}
owned = []
owned_files = []
started = time.monotonic()
deadline = started + 35
last_rcon = 0


def check(name, passed, evidence):
    result['cases'].append({'name': name, 'passed': bool(passed), 'evidence': evidence})


def rcon(command):
    global last_rcon
    time.sleep(max(0, 1.08 - (time.monotonic() - last_rcon)))
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.settimeout(.65)
        sock.sendto(b'\xff' * 4 + ('rcon local-khz-benchmark ' + command).encode('ascii'), ('127.0.0.1', SERVER_PORT))
        last_rcon = time.monotonic()
        packets = []
        try:
            while True:
                data, sender = sock.recvfrom(65536)
                if sender == ('127.0.0.1', SERVER_PORT):
                    if data.startswith(b'\xff' * 4):
                        data = data[4:]
                    if data.startswith(b'print\n'):
                        data = data[6:]
                    packets.append(data.decode('latin1'))
        except socket.timeout:
            pass
    reply = ''.join(packets)
    result['commands'].append({'command': command, 'reply': reply})
    return reply


class Endpoint(http.server.BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'
    def log_message(self, *unused):
        pass
    def handle(self):
        try:
            super().handle()
        except ConnectionResetError:
            pass  # WinHTTP deliberately closes the consumed fixture response.
    def do_POST(self):
        body = self.rfile.read(int(self.headers.get('Content-Length', '0')))
        entry = {'path': self.path, 'body': body.decode('latin1'), 'authorization': self.headers.get('Authorization'),
                 'content_type': self.headers.get('Content-Type')}
        entry['valid'] = self.path == '/sdk' and body == b'command=HELO&literal=A%2BB+%26%3D%2525' and entry['authorization'] == 'fixture-token'
        result['http_requests'].append(entry)
        response = b'status=okay&literal=A%2BB+%26%3D%2525' if entry['valid'] else b'status=failed'
        self.send_response(200 if entry['valid'] else 400)
        self.send_header('Content-Length', str(len(response)))
        self.send_header('Content-Type', 'application/x-www-form-urlencoded')
        self.end_headers()
        self.wfile.write(response)


def launch(name, arguments):
    log = (OUTPUT / (name + '-stdio.log')).open('wb')
    executable = RUNTIME / (name + '.exe')
    process = subprocess.Popen([str(executable), *arguments], cwd=RUNTIME, stdout=log, stderr=subprocess.STDOUT,
                               creationflags=subprocess.CREATE_NO_WINDOW)
    owned.append((process, log))
    result['processes'].append({'pid': process.pid, 'executable': str(executable), 'arguments': arguments})
    (OUTPUT / 'processes.json').write_text(json.dumps(result['processes'], indent=2))
    return process


def native_log(process):
    if 'server' in process.args[0]:
        return RUNTIME / 'benchmarks' / f'server-{SERVER_PORT}' / 'main' / 'khz_server.log'
    return RUNTIME / 'benchmarks' / f'client-{CLIENT_PORT}' / 'main' / f'khz_bench_{process.pid}.log'


server = client = endpoint = None
log_offsets = {}
old_logs = {}
try:
    for port in (SERVER_PORT, CLIENT_PORT):
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as reserve:
            reserve.bind(('127.0.0.1', port))
    for relative in ('cod4x1khz_server.exe', 'cod4x1khz_bench.exe', 'plugins/khz_regression.dll', 'main/zz_cod4x1khz_bots.iwd'):
        path = RUNTIME / relative
        if path.is_file():
            result['sha256'][str(path)] = hashlib.sha256(path.read_bytes()).hexdigest()
    for relative in ('sdk/tests/regression_plugin.c', 'sdk/tests/plugin_fixture.gsc', 'sdk/tests/native_thread_probe.inc',
                     'sdk/tests/native_http_probe.inc', 'sdk/tests/native_stats_probe.inc', 'scripts/plugin_native_probe.py',
                     'native/command_arguments.hpp', 'engine/src/qcommon/cmd.cpp', 'engine/src/server_mp/sv_main_pc_mp.cpp'):
        path = PROJECT / relative
        result['sha256'][str(path)] = hashlib.sha256(path.read_bytes()).hexdigest()
    endpoint = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Endpoint)
    threading.Thread(target=endpoint.serve_forever, daemon=True).start()
    config_name = 'khz_sdk_http_' + time.strftime('%Y%m%d_%H%M%S') + '.cfg'
    config = RUNTIME / 'benchmarks' / f'server-{SERVER_PORT}' / 'main' / config_name
    config.parent.mkdir(parents=True, exist_ok=True)
    config_text = f'khz_fixture_http_start "http://127.0.0.1:{endpoint.server_port}/sdk"\n'
    with config.open('x', encoding='ascii') as stream:
        stream.write(config_text)
    owned_files.append(config)
    (OUTPUT / config_name).write_text(config_text, encoding='ascii')
    result['http_fixture_config'] = {'path': str(config), 'contents': config_text, 'sha256': hashlib.sha256(config.read_bytes()).hexdigest()}
    server_log = RUNTIME / 'benchmarks' / f'server-{SERVER_PORT}' / 'main' / 'khz_server.log'
    log_offsets[server_log] = server_log.stat().st_size if server_log.exists() else 0
    for path in (server_log, server_log.parent / 'games_mp.log', server_log.parent.parent / 'administration' / 'audit.log'):
        old_logs[path] = path.read_bytes() if path.exists() else b''
    server = launch('cod4x1khz_server', ['+set', 'dedicated', '1', '+set', 'net_ip', '127.0.0.1', '+set', 'net_port', str(SERVER_PORT),
        '+set', 'khz_benchmark', '1', '+set', 'khz_test_controls', '1', '+exec', 'server.cfg', '+set', 'khz_bots', '0',
        '+set', 'khz_bot_count', '0', '+loadPlugin', 'khz_regression', '+set', 'khz_plugin_tests', '1', '+map', 'mp_killhouse'])
    while time.monotonic() < deadline - 24:
        try:
            status = query(SERVER_PORT, 'rcon local-khz-benchmark khz_json', 'KHZ_JSON', .4)
            result['initial_server'] = status
            break
        except (TimeoutError, OSError):
            if server.poll() is not None:
                raise RuntimeError('Owned server exited during startup')
            time.sleep(1.08)
    else:
        raise TimeoutError('Server did not become ready within startup scope')
    client = launch('cod4x1khz_bench', ['+set', 'net_port', str(CLIENT_PORT), '+set', 'khz_test_controls', '1', '+exec', 'client.cfg',
        '+set', 'com_introPlayed', '1', '+set', 'name', 'sdk-native-fixture', '+connect', f'127.0.0.1:{SERVER_PORT}'])
    replies = {}
    for command in ('khz_fixture_aliases', 'khz_fixture_identity', 'khz_fixture_http_forms', 'khz_fixture_threads_start',
                    f'khz_fixture_http_start \"http://127.0.0.1:{endpoint.server_port}/sdk\"',
                    r'khz_command_arguments_test "http://127.0.0.1:65535/exact?x=1" "" "literal \"quotes\"" tail\ "one; khz_argument_injected" "// comment /* literal */"'):
        replies[command.split()[0]] = rcon(command)
    result['client_states'] = []
    while time.monotonic() < deadline - 12:
        try:
            state = query(CLIENT_PORT, 'khz_state', 'KHZ_STATE', .3)
            if not result['client_states'] or result['client_states'][-1].get('connection') != state.get('connection'):
                result['client_states'].append(state)
            if state.get('connection') == 9:
                result['active_client'] = state
                break
        except (TimeoutError, OSError):
            if client.poll() is not None:
                result['client_admission_error'] = 'Owned headless client exited during startup'
                break
        time.sleep(.1)
    else:
        result['client_admission_error'] = 'Human admission did not become active within startup scope'
    before = None
    try:
        before = query(CLIENT_PORT, 'khz_ui status', 'KHZ_UI', .5)['profile']
        result['profile_before'] = before
    except (KeyError, OSError, TimeoutError) as error:
        result['profile_read_error'] = str(error)
    commands = ('khz_fixture_policy 0', 'khz_fixture_stats 0',
        'khz_fixture_threads_report', 'khz_fixture_http_report', 'khz_fixture_report')
    for command in commands:
        if time.monotonic() >= deadline - 3:
            raise TimeoutError('Native fixture scope exceeded its command budget')
        replies[command.split()[0]] = rcon(command)
    after = None
    try:
        after = query(CLIENT_PORT, 'khz_ui status', 'KHZ_UI', .5)['profile']
        result['profile_after'] = after
    except (KeyError, OSError, TimeoutError):
        pass
    result['profile_observation'] = {'before': before, 'after': after,
        'scope': 'Live match stats may change between these observations. Command-local received-stat restoration is the fixture oracle; a whole-profile preservation claim requires a parked fixture.'}
    expectations = {
      'aliases': ('khz_fixture_aliases', ['deprecated=1', 'loaded=1', 'refreshed=1', 'missing=1', 'clienttoslot=1', 'steam_alias=1', 'main=1']),
      'identity': ('khz_fixture_identity', ['conversions=1', 'guid_golden=2310346614158149229', 'official_provider=unavailable']),
      'HTTP forms and owner blocking rejection': ('khz_fixture_http_forms', ['form=1', 'repeated=1', 'main_blocking_rejected=1']),
      'native threads and cooperative exit': ('khz_fixture_threads_report', ['start=1', 'raw=1', 'worker_owner=1', 'arguments=1', 'setup=1', 'finished=1', 'callbacks=1', 'callback_owner=1', 'exit_unreachable=1', 'tick_rejected=1']),
      'command membership policy': ('khz_fixture_policy', ['before=0', 'after=1', 'probes=1']),
      'received native stat boundaries/restoration': ('khz_fixture_stats', ['roundtrip=1', 'restored=1', 'invalid_rejected=1', 'all_received_fields=1']),
      'Exact RCON arguments and nested dispatch': ('khz_command_arguments_test', ['incoming=1', 'tokens=1', 'nested=1', 'restored=1', 'bounds=1', 'appended=0']),
      'HTTP wrapper job and owner completion': ('khz_fixture_http_report', ['active=0', 'worker_response=1', 'completions=1', 'owner=1']),
      'authoritative callbacks and async job': ('khz_fixture_report', ['gaps=0', 'work=1', 'completions=1', 'allocation_rejected=1', 'policy_probes=1'])}
    for name, (command, tokens) in expectations.items():
        reply = replies[command]
        check(name, all(token in reply for token in tokens), reply)
    callback_counts = dict((key, int(value)) for key, value in re.findall(r'(frames|ticks|gaps|moves)=(\d+)', replies['khz_fixture_report']))
    check('1 ms frame/tick callbacks match with actual human commands', callback_counts.get('frames', 0) > 0 and
          callback_counts.get('frames') == callback_counts.get('ticks') and callback_counts.get('moves', 0) > 0 and callback_counts.get('gaps') == 0, callback_counts)
    check('HTTP actual form POST reached controlled endpoint exactly once', len(result['http_requests']) == 1 and result['http_requests'][0]['valid'], result['http_requests'])
    telemetry_reply = rcon('khz_json')
    marker = telemetry_reply.find('KHZ_JSON ')
    if marker >= 0:
        result['final_server'] = json.JSONDecoder().raw_decode(telemetry_reply[marker + 9:])[0]
except Exception as error:
    result['error'] = str(error)
    check('Bounded native fixture run completed', False, str(error))
finally:
    if client and client.poll() is None:
        try:
            query(CLIENT_PORT, 'khz_ui quit', 'KHZ_UI', .25)
        except (OSError, TimeoutError):
            pass
    if server and server.poll() is None:
        try:
            rcon('quit')
        except OSError:
            pass
    for process, log in owned:
        try:
            process.wait(timeout=1)
        except subprocess.TimeoutExpired:
            process.terminate()
            process.wait(timeout=2)
        log.close()
        source = native_log(process)
        if source.exists():
            data = source.read_bytes()
            previous = old_logs.get(source, b'')
            if previous and data.startswith(previous):
                data = data[len(previous):]
            # Native logging opens with truncation; retain the current owned process log.
            destination = OUTPUT / ('server-native.log' if process is server else 'client-native.log')
            destination.write_bytes(data)
            result['sha256'][str(destination)] = hashlib.sha256(data).hexdigest()
    if endpoint:
        endpoint.shutdown()
        endpoint.server_close()
    for path in owned_files:
        path.unlink()
    server_text = (OUTPUT / 'server-native.log').read_text(encoding='latin1') if (OUTPUT / 'server-native.log').exists() else ''
    # Plugin print journals intentionally drain after the redirect closes.
    # Validate their current-run native log records, not empty RCON packets.
    native_cases = {
        'aliases': ('KHZ_PLUGIN_ALIAS_FIXTURE', ['deprecated=1', 'loaded=1', 'refreshed=1', 'missing=1', 'clienttoslot=1', 'steam_alias=1', 'main=1']),
        'identity': ('KHZ_PLUGIN_IDENTITY_FIXTURE', ['conversions=1', 'guid_golden=2310346614158149229', 'official_provider=unavailable']),
        'HTTP forms and owner blocking rejection': ('KHZ_PLUGIN_HTTP_FORM_FIXTURE', ['form=1', 'repeated=1', 'main_blocking_rejected=1']),
        'native threads and cooperative exit': ('KHZ_PLUGIN_THREAD_FIXTURE', ['start=1', 'raw=1', 'worker_owner=1', 'arguments=1', 'setup=1', 'finished=1', 'callbacks=1', 'callback_owner=1', 'exit_unreachable=1', 'tick_rejected=1']),
        'command membership policy': ('KHZ_PLUGIN_POLICY_FIXTURE', ['before=0', 'after=1', 'probes=1']),
        'received native stat boundaries/restoration': ('KHZ_PLUGIN_STATS_FIXTURE', ['roundtrip=1', 'restored=1', 'invalid_rejected=1', 'all_received_fields=1']),
        'Exact RCON arguments and nested dispatch': ('KHZ_ARGUMENT_FIXTURE', ['incoming=1', 'tokens=1', 'nested=1', 'restored=1', 'bounds=1', 'appended=0']),
        'HTTP wrapper job and owner completion': ('KHZ_PLUGIN_HTTP_FIXTURE', ['active=0', 'worker_response=1', 'completions=1', 'owner=1']),
        'authoritative callbacks and async job': ('KHZ_PLUGIN_FIXTURE', ['gaps=0', 'work=1', 'completions=1', 'allocation_rejected=1', 'policy_probes=1'])}
    for case in result['cases']:
        if case['name'] in native_cases:
            marker, tokens = native_cases[case['name']]
            lines = [line[line.index(marker):] for line in server_text.splitlines() if marker in line]
            if not lines and case['name'] == 'Exact RCON arguments and nested dispatch':
                record = replies.get('khz_command_arguments_test', '')
                case['passed'] = all(token in record for token in tokens)
                case['evidence'] = {'source': 'RCON response', 'record': record}
            else:
                case['passed'] = bool(lines) and all(token in lines[-1] for token in tokens)
                case['evidence'] = {'source': 'server-native.log', 'record': lines[-1] if lines else None}
        elif case['name'] == '1 ms frame/tick callbacks match with actual human commands':
            lines = [line for line in server_text.splitlines() if 'KHZ_PLUGIN_FIXTURE frames=' in line]
            counts = dict((key, int(value)) for key, value in re.findall(r'(frames|ticks|gaps|moves)=(\d+)', lines[-1] if lines else ''))
            case['passed'] = counts.get('frames', 0) > 0 and counts.get('frames') == counts.get('ticks') and counts.get('moves', 0) > 0 and counts.get('gaps') == 0
            case['evidence'] = {'source': 'server-native.log', 'counts': counts}
    check('Native script function and usercall', 'KHZ_PLUGIN_SCRIPT_FIXTURE dynamic function and usercall passed' in server_text, 'server-native.log')
    check('Native script entity method and usercall', 'KHZ_PLUGIN_SCRIPT_FIXTURE dynamic method passed' in server_text, 'server-native.log')
    game_log = RUNTIME / 'benchmarks' / f'server-{SERVER_PORT}' / 'main' / 'games_mp.log'
    admin_log = RUNTIME / 'benchmarks' / f'server-{SERVER_PORT}' / 'administration' / 'audit.log'
    for source, marker, label in ((game_log, 'KHZ_PLUGIN_GAME_LOG_FIXTURE', 'game'), (admin_log, 'KHZ_PLUGIN_ADMIN_LOG_FIXTURE', 'administration')):
        data = source.read_bytes() if source.exists() else b''
        previous = old_logs.get(source, b'')
        if previous and data.startswith(previous):
            data = data[len(previous):]
        (OUTPUT / (label + '.log')).write_bytes(data)
        check('Native timestamped ' + label + ' logging', marker.encode() in data, data.decode('latin1')[-3000:])
    result['elapsed_seconds'] = time.monotonic() - started
    result['passed'] = bool(result['cases']) and all(case['passed'] for case in result['cases'])
    result['owned_processes_exited'] = all(process.poll() is not None for process, log in owned)
    (OUTPUT / 'result.json').write_text(json.dumps(result, indent=2), encoding='utf8')
    print(json.dumps({'output': str(OUTPUT), 'passed': result['passed'], 'elapsed_seconds': result['elapsed_seconds'], 'cases': result['cases'], 'error': result.get('error')}, indent=2))
raise SystemExit(0 if result['passed'] else 1)
