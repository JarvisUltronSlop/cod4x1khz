"""Compile source-owned checkpoint fragments without deploying a game build.

This verifies native types/visibility only. It neither registers providers nor
attests actual capture, restore or full-world replay.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess
from physics_guard_inventory import PROFILE, active_source, comments_removed

PROJECT = Path(__file__).resolve().parent.parent
FRAGMENTS = (
    ('script/scr_vm.cpp', 'khz_server_vm_checkpoint.inl'),
    ('script/scr_variable.cpp', 'khz_server_variables_checkpoint.inl'),
    ('script/scr_memorytree.cpp', 'khz_server_mt_checkpoint.inl'),
    ('script/scr_stringlist.cpp', 'khz_server_sl_checkpoint.inl'),
    ('game/enthandle.cpp', 'khz_server_enthandles_checkpoint.inl'),
    ('qcommon/dobj_management.cpp', 'khz_server_dobj_checkpoint.inl'),
    ('xanim/xanim.cpp', 'khz_server_animation_checkpoint.inl'),
    ('physics/phys_ode.cpp', 'khz_server_physics_checkpoint.inl'),
    ('physics/ode/ode.cpp', 'khz_server_ode_checkpoint.inl'),
    ('physics/ode/collision_kernel.cpp', 'khz_server_ode_colliders_checkpoint.inl'),
    ('physics/ode/timer.cpp', 'khz_server_ode_timers_checkpoint.inl'),
    ('physics/phys_coll_capsulebrush.cpp', 'khz_server_physics_capsule_checkpoint.inl'),
    ('physics/ode/error.cpp', 'khz_server_ode_errors_checkpoint.inl'),
    ('physics/ode/quickstep.cpp', 'khz_server_ode_quickstep_checkpoint.inl'),
    ('physics/phys_contacts.cpp', 'khz_server_physics_steps_checkpoint.inl'),
    ('game_mp/g_main_mp.cpp', 'khz_server_world_main_checkpoint.inl'),
    ('game_mp/g_scr_main_mp.cpp', 'khz_server_world_script_checkpoint.inl'),
    ('game/g_hudelem.cpp', 'khz_server_world_hud_checkpoint.inl'),
    ('game_mp/g_misc_mp.cpp', 'khz_server_world_turrets_checkpoint.inl'),
    ('game_mp/g_vehicles_mp.cpp', 'khz_server_world_vehicles_checkpoint.inl'),
    ('game/g_missile.cpp', 'khz_server_world_missiles_checkpoint.inl'),
    ('game/g_mover.cpp', 'khz_server_world_movers_checkpoint.inl'),
    ('game/g_scr_vehicle.cpp', 'khz_server_world_vehicle_physics_checkpoint.inl'),
    ('universal/dvar.cpp', 'khz_server_dvars_checkpoint.inl'),
    ('khz/khz_runtime.cpp', 'khz_server_schedule_checkpoint.inl'),
    ('khz/khz_runtime.cpp', 'khz_server_runtime_checkpoint.inl'),
    ('khz/khz_bots.cpp', 'khz_bots_checkpoint.inl'),
    ('khz/khz_admin_server.cpp', 'khz_server_admin_checkpoint.inl'),
    ('server_mp/sv_snapshot_mp.cpp', 'khz_server_snapshot_checkpoint.inl'),
    ('qcommon/net_chan_mp.cpp', 'khz_server_transport_checkpoint.inl'),
    ('qcommon/cm_load.cpp', 'khz_server_collision_load_checkpoint.inl'),
    ('qcommon/cm_world.cpp', 'khz_server_collision_world_checkpoint.inl'),
    ('server/sv_world.cpp', 'khz_server_collision_query_checkpoint.inl'),
    ('khz/khz_host.cpp', 'khz_server_checkpoint_owner.inl'),
    ('universal/com_memory.cpp', 'khz_server_checkpoint_hunk.inl'),
)

# Compile every native TU containing an opt-in marker or a scoped Physics
# guard, not just provider-owning TUs. Header hooks compile via their callers.
NATIVE_OPT_IN = re.compile(r'COD4X1KHZ_SERVER_CHECKPOINT|KHZ_SERVER_PHYSICS_GUARD_')


def read_native(path):
    raw = path.read_bytes()
    try:
        return raw.decode('utf-8-sig')
    except UnicodeDecodeError:
        # Preserve every original byte in legacy comment/string encodings;
        # wrapper inclusion still compiles the unchanged original source.
        return raw.decode('latin1')


def write_generated(path, text):
    # Keep unchanged wrapper mtimes so native dependency tracking can compile
    # only the declarations/sources actually repaired on subsequent runs.
    if not path.exists() or path.read_text(encoding='utf8') != text:
        path.write_text(text, encoding='utf8')


def symbol_definitions(path):
    """Read COFF external definitions, including MSVC bigobj; no linker claim."""
    data = path.read_bytes()
    if len(data) < 20:
        raise ValueError(f'Truncated COFF object: {path}')
    if data[:4] == b'\x00\x00\xff\xff':
        if len(data) < 56 or struct.unpack_from('<H', data, 4)[0] < 2:
            raise ValueError(f'Unsupported COFF bigobj header: {path}')
        sections, symbols, count = struct.unpack_from('<III', data, 44)
        section_base, size, section_fmt, storage_at = 56, 20, '<i', 18
    else:
        machine, sections, _, symbols, count, optional, _ = struct.unpack_from('<HHIIIHH', data)
        if machine not in (0x14c, 0x8664):
            raise ValueError(f'Unsupported COFF machine: {path}')
        section_base, size, section_fmt, storage_at = 20 + optional, 18, '<h', 16
    if sections > 100000 or count > 10000000 or section_base + sections * 40 > len(data) or symbols + count * size + 4 > len(data):
        raise ValueError(f'Invalid bounded COFF extents: {path}')
    strings = symbols + count * size
    string_size = struct.unpack_from('<I', data, strings)[0]
    if string_size < 4 or strings + string_size > len(data):
        raise ValueError(f'Invalid COFF string table: {path}')
    flags = [struct.unpack_from('<I', data, section_base + i * 40 + 36)[0] for i in range(sections)]
    records, selections = [], {}
    index = 0
    while index < count:
        at = symbols + index * size
        encoded = data[at:at + 8]
        if encoded[:4] == b'\x00' * 4:
            offset = struct.unpack_from('<I', encoded, 4)[0]
            if offset < 4 or offset >= string_size:
                raise ValueError(f'Invalid COFF symbol name: {path}')
            end = data.find(b'\x00', strings + offset, strings + string_size)
            if end < 0:
                raise ValueError(f'Unterminated COFF symbol name: {path}')
            name = data[strings + offset:end].decode('utf8', errors='replace')
        else:
            name = encoded.split(b'\x00', 1)[0].decode('utf8', errors='replace')
        section = struct.unpack_from(section_fmt, data, at + 12)[0]
        type_at = 16 if size == 20 else 14
        kind = struct.unpack_from('<H', data, at + type_at)[0]
        storage, auxiliary = data[at + storage_at:at + storage_at + 2]
        if index + 1 + auxiliary > count:
            raise ValueError(f'COFF auxiliary symbols escape table: {path}')
        if storage == 3 and kind == 0 and auxiliary and 0 < section <= sections:
            # Section definitions identify COMDAT's actual selection policy.
            selections[section] = data[at + size + 14]
        if storage == 2 and 0 < section <= sections:
            records.append((name, section))
        index += 1 + auxiliary
    for name, section in records:
        if flags[section - 1] & 0x1000:
            selection = selections.get(section)
            if selection is None:
                raise ValueError(f'COMDAT without section selection: {path}:{name}')
            if selection != 1:  # ANY/SAME_SIZE/EXACT_MATCH/ASSOCIATIVE/etc coalesce.
                continue
        yield name


def duplicate_definitions(build):
    definitions, duplicates = {}, {}
    objects = sorted(path for path in build.rglob('*.obj') if 'khz_checkpoint_staging.dir' in path.parts)
    if not objects:
        raise ValueError('Staging produced no native object files')
    for path in objects:
        for name in symbol_definitions(path):
            if name in definitions and definitions[name] != str(path):
                duplicates.setdefault(name, [definitions[name]]).append(str(path))
            else:
                definitions[name] = str(path)
    return len(objects), definitions, duplicates


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--native-hooks', action='store_true', help='Compile the actual dedicated opt-in hooks too')
    parser.add_argument('--parallel', type=int, default=4)
    args = parser.parse_args()
    if not 1 <= args.parallel <= 4:
        parser.error('--parallel must be between 1 and 4')
    suffix = '-hooks' if args.native_hooks else ''
    source = PROJECT / ('build/checkpoint-staging-src' + suffix)
    build = PROJECT / ('build/checkpoint-staging' + suffix)
    source.mkdir(parents=True, exist_ok=True)
    build.mkdir(parents=True, exist_ok=True)
    result_path = build / 'staging-result.json'
    # Never leave a previous PASS available if this attempt fails during
    # wrapper generation, configuration, compilation or symbol validation.
    result_path.write_text(json.dumps({'completed': False, 'passed': False,
        'native_hooks': args.native_hooks, 'phase': 'source_generation',
        'full_link_attestation': False, 'native_restore_attestation': False,
        'full_world_replay_attestation': False}, indent=2), encoding='utf8')
    files = []
    grouped = {}
    for native, fragment in FRAGMENTS:
        grouped.setdefault(native, []).append(fragment)
    profile = dict(PROFILE, COD4X1KHZ=True)
    profile['COD4X1KHZ_SERVER_CHECKPOINT'] = args.native_hooks
    touched = []
    for path in sorted((PROJECT / 'engine/src').rglob('*.cpp')):
        native_source = read_native(path)
        if NATIVE_OPT_IN.search(comments_removed(native_source)):
            native = path.relative_to(PROJECT / 'engine/src').as_posix()
            grouped.setdefault(native, [])
            touched.append(native)
    manifest = []
    for index, (native, fragments) in enumerate(grouped.items()):
        wrapper = source / f'part{index:02}.cpp'
        native_path = PROJECT / 'engine/src' / native
        active = active_source(read_native(native_path), profile, preserve_includes=True)
        already = re.findall(r'#\s*include\s*[<"](?:khz/)?(khz_[^>"/]+\.inl)[>"]', active)
        included = [fragment for fragment in fragments if fragment not in already]
        for fragment in fragments:
            if already.count(fragment) > 1:
                raise ValueError(f'Duplicate active native include: {native}: {fragment}')
        write_generated(wrapper, f'#include "{native_path.as_posix()}"\n'
                           + ''.join(f'#include "{(PROJECT / "engine/src/khz" / fragment).as_posix()}"\n'
                                     for fragment in included))
        files.append(wrapper.as_posix())
        manifest.append({'native': native, 'native_sha256': hashlib.sha256(native_path.read_bytes()).hexdigest(),
                         'native_includes': already, 'wrapper_includes': included})
    files += [(PROJECT / f'engine/src/khz/{name}.cpp').as_posix() for name in
              ('khz_server_checkpoint', 'khz_server_network_checkpoint')]
    includes = [(PROJECT / name).as_posix() for name in
                ('engine/src', 'engine/deps', 'engine/deps/msslib', 'sdk/include', 'native',
                 '.deps/dxsdk/9.29.952.8/build/native/include', 'build/Release/_deps/tracy-src/public')]
    write_generated(source / 'CMakeLists.txt',
        'cmake_minimum_required(VERSION 3.25)\nproject(khz_checkpoint_staging LANGUAGES C CXX)\n'
        'set(CMAKE_CXX_STANDARD 20)\nset(CMAKE_MSVC_RUNTIME_LIBRARY MultiThreaded)\n'
        'add_library(khz_checkpoint_staging OBJECT\n' + ''.join(f'  "{p}"\n' for p in files) + ')\n'
        'target_include_directories(khz_checkpoint_staging PRIVATE\n' + ''.join(f'  "{p}"\n' for p in includes) + ')\n'
        'target_compile_definitions(khz_checkpoint_staging PRIVATE WIN32 _WINDOWS NDEBUG KISAK_MP '
        'KISAK_DEDICATED DEDICATED _CONSOLE _MBCS COD4X1KHZ=1 _CRT_SECURE_NO_WARNINGS '
        '_CRT_NONSTDC_NO_DEPRECATE TRACY_ON_DEMAND TRACY_ONLY_LOCALHOST KisakCOD_dedi_EXPORTS '
        + ('COD4X1KHZ_SERVER_CHECKPOINT=1' if args.native_hooks else '') + ')\n'
        'target_compile_options(khz_checkpoint_staging PRIVATE /fp:precise /W3)\n')
    subprocess.run(['cmake', '-S', str(source), '-B', str(build), '-G', 'Visual Studio 17 2022',
                    '-A', 'Win32'], check=True)
    subprocess.run(['cmake', '--build', str(build), '--config', 'Release', '--parallel', str(args.parallel)], check=True)
    object_count, definitions, duplicates = duplicate_definitions(build)
    result = {'completed': True, 'passed': not duplicates, 'native_hooks': args.native_hooks,
              'translation_units': len(files), 'objects': object_count,
              'strong_external_definitions': len(definitions), 'duplicate_strong_symbols': duplicates,
              'native_touched': touched, 'manifest': manifest, 'full_link_attestation': False,
              'native_restore_attestation': False, 'full_world_replay_attestation': False}
    result_path.write_text(json.dumps(result, indent=2), encoding='utf8')
    if duplicates:
        print(json.dumps({'duplicate_strong_symbols': duplicates}, indent=2))
        raise ValueError('Staging object definitions contain duplicate strong symbols')
    print(f'Staged native compile passed: {len(files)} translation units; hooks={args.native_hooks}; '
          f'{len(definitions)} strong definitions checked; no full link/deployment/restoration claim.')


if __name__ == '__main__':
    main()
