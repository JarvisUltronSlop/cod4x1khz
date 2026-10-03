"""Bind an explicitly selected native fixture to an archived coherent build.

This checks bytes and the bounded fixture command inventory. It never launches
a process, compiles, or treats source inventory as semantic proof.
"""
import hashlib
import json
from pathlib import Path
import re


def inspect_fixture_registration(source, native, pinned):
    """Check literal top-level OnInit inventory; never claim runtime coverage."""
    tokens = re.compile(r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|/\*[\s\S]*?\*/|//[^\n]*')
    blank = lambda match: ''.join('\n' if char == '\n' else ' ' for char in match.group())
    comments_removed = tokens.sub(lambda match: blank(match) if match.group().startswith(('/', '/*')) else match.group(), source)
    masked = tokens.sub(blank, source)
    entry = re.search(r'\bOnInit\s*\(\s*void\s*\)\s*\{', masked)
    if not entry:
        raise ValueError('Regression fixture has no supported OnInit body')
    begin, cursor, depth = entry.end(), entry.end(), 1
    while cursor < len(masked) and depth:
        depth += (masked[cursor] == '{') - (masked[cursor] == '}')
        cursor += 1
    if depth:
        raise ValueError('Regression fixture OnInit braces are incomplete')
    body, code = comments_removed[begin:cursor - 1], masked[begin:cursor - 1]
    literal = re.compile(r'Plugin_AddCommand\s*\(\s*"([A-Za-z0-9_-]+)"\s*,\s*([A-Za-z_][A-Za-z0-9_]*)\s*,\s*(\d+)\s*\)')
    commands = []
    for invocation in re.finditer(r'\bPlugin_AddCommand\s*\(', code):
        call = literal.match(body, invocation.start())
        if not call or code[:invocation.start()].count('{') != code[:invocation.start()].count('}'):
            raise ValueError('Regression command inventory requires literal, unconditional top-level registrations')
        commands.append({'name': call.group(1), 'callback': call.group(2), 'power': int(call.group(3))})
    native_limit = re.search(r'return\s+add\(commands_,\s*owner,\s*name,\s*Kind::Command,\s*callback,\s*false,\s*power,\s*(\d+),\s*slot\)', native)
    pinned_limit = re.search(r'pluginCmd_t\s+cmd\s*\[\s*(\d+)\s*\]', pinned)
    if not native_limit or not pinned_limit or int(native_limit.group(1)) != 20 or int(pinned_limit.group(1)) != 20:
        raise ValueError('Pinned/native per-plugin command capacity no longer equals 20; review fixture before launch')
    if len(commands) != 20:
        raise ValueError(f'Regression fixture registers {len(commands)} commands; expected exactly 20 before native launch')
    names = [command['name'].lower() for command in commands]
    if len(set(names)) != len(names):
        raise ValueError('Regression fixture has duplicate command names before native launch')
    required = {'khz_fixture_identity', 'khz_fixture_net', 'khz_fixture_net_admission', 'khz_fixture_net_ipban', 'khz_fixture_udp'}
    if not required.issubset(names):
        raise ValueError('Regression fixture is missing a required identity/network command')
    identity = next(command for command in commands if command['name'].lower() == 'khz_fixture_identity')
    if identity['callback'] != 'identity_dispatch' or any(command['power'] != 100 for command in commands):
        raise ValueError('Regression fixture identity dispatcher/power requires review before native launch')
    return {'observed_count': len(commands), 'native_capacity': 20, 'pinned_array_capacity': 20,
            'commands': commands, 'check': 'Archived literal initialization inventory only; actual command execution remains required'}


def bind_archive(project, archive_name, result, binaries, compiled_sources,
                 packages=('runtime/main/zz_cod4x1khz_bots.iwd',)):
    project = Path(project).resolve()
    archive_path = Path(archive_name)
    if not archive_path.is_absolute():
        archive_path = project / archive_path
    archive_path = archive_path.resolve()
    if not archive_path.is_relative_to(project):
        raise ValueError('Build archive must remain beneath the project directory')
    archive = json.loads(archive_path.read_text(encoding='utf-8-sig'))
    build = archive.get('build')
    if type(build) is not int or build <= 0 or archive.get('tests', {}).get('passed') is not True:
        raise ValueError('Fixture requires a numbered archive with passed native build suites')
    result['build'] = build
    result['build_archive'] = str(archive_path)
    result.setdefault('sha256', {})[str(archive_path)] = hashlib.sha256(archive_path.read_bytes()).hexdigest()
    helper_path = Path(__file__).resolve()
    result['sha256'][str(helper_path)] = hashlib.sha256(helper_path.read_bytes()).hexdigest()
    mismatches = []
    for group, names in (('binary_sha256', binaries), ('source_sha256', compiled_sources),
                         ('runtime_asset_packages', packages)):
        for name in names:
            path = (project / name).resolve()
            if not path.is_relative_to(project):
                raise ValueError('Fixture input must remain beneath the project directory')
            actual = hashlib.sha256(path.read_bytes()).hexdigest()
            result['sha256'][str(path)] = actual
            if archive.get(group, {}).get(name) != actual:
                mismatches.append(name)
    result['archive_mismatches'] = mismatches
    if mismatches:
        raise RuntimeError('Fixture refused mismatched archived bytes: ' + ', '.join(mismatches))
    if 'sdk/tests/regression_plugin.c' in compiled_sources:
        inputs = ('sdk/tests/regression_plugin.c', 'native/plugin_registry.hpp', 'upstream/cod4x_server/src/plugin_handler.h')
        text = []
        for name in inputs:
            path = project / name
            actual = hashlib.sha256(path.read_bytes()).hexdigest()
            result['sha256'][str(path)] = actual
            if name != inputs[2] and archive.get('source_sha256', {}).get(name) != actual:
                raise RuntimeError('Fixture registration check requires exact archived source bytes: ' + name)
            text.append(path.read_text(encoding='utf-8'))
        result['fixture_registration'] = inspect_fixture_registration(*text)
    return build
