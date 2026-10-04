"""Load every installed stock MP zone in an isolated owned DX9 inventory client.

This is shader inventory, not Vulkan stock-rendering acceptance. Original game
data and interactive settings stay unchanged. Exported bytecode stays private.
"""
from datetime import datetime, timezone
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import time

from native_checkpoint_capture_probe import ManagedHostFixture
from native_fov_probe import interactive_preferences, wait_recovery


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', type=int, default=28995)
    parser.add_argument('--maps', nargs='*', help='Default: all installed stock multiplayer zones')
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    output = root / 'results' / ('vulkan-stock-shader-inventory-' + datetime.now(timezone.utc).strftime('%Y%m%d-%H%M%S'))
    output.mkdir()
    source = root / 'bin/Release/cod4x1khz.exe'
    binary = root / 'runtime/cod4x1khz.exe'
    preserved = output / 'preserved-runtime-client.exe'
    shutil.copy2(binary, preserved)
    tested_hash = hashlib.sha256(source.read_bytes()).hexdigest()
    maps = args.maps or sorted(p.stem for p in (root / 'runtime/zone/english').glob('mp_*.ff') if not p.stem.endswith('_load'))
    if not maps or any(not (root / 'runtime/zone/english' / (m + '.ff')).is_file() for m in maps):
        raise ValueError('Inventory requires installed stock map zones')
    before = interactive_preferences(root)
    # The updater admits only installed project executable names. Temporarily
    # stage this diagnostic in the existing runtime location and restore it;
    # the user's root executable remains the unchanged DX9 fallback throughout.
    shutil.copy2(source, binary)
    fixture = ManagedHostFixture(root, output, args.port, budget=max(120, 35 * len(maps)), client_binary=binary,
                                 client_settings={'r_vkShaderInventory': '1', 'com_maxfps': '125'})
    fixture.report.update(scope=__doc__, requested_maps=maps, maps=[], root_binary_sha256=hashlib.sha256((root / 'cod4x1khz.exe').read_bytes()).hexdigest())
    captured = fixture.home / 'vulkan/shader-inventory'
    try:
        fixture.start()
        for map_name in maps:
            start = time.monotonic()
            current = fixture.sample()
            if current['state'].get('map') != map_name:
                fixture.call('khz_ui server map ' + map_name)
                wait_recovery(fixture, 'shader inventory ' + map_name, map_name, current, changed_map=True)
            row = {'map': map_name, 'elapsed': time.monotonic() - start,
                   'unique_bytecode_files': len(list(captured.glob('*.dxbc'))), 'state': fixture.sample()['state']}
            fixture.report['maps'].append(row)
            print(f"{map_name}: {row['unique_bytecode_files']} unique bytecodes collected", flush=True)
        if not (captured / 'inventory.csv').is_file():
            raise RuntimeError('Native shader inventory emitted no metadata')
        shutil.copytree(captured, output / 'shaders')
        fixture.report['passed'] = True
    except Exception as error:
        fixture.report['errors'].append(str(error))
    finally:
        fixture.close()
        if hashlib.sha256(binary.read_bytes()).hexdigest() == tested_hash:
            shutil.copy2(preserved, binary)
            fixture.report['runtime_binary_restored'] = hashlib.sha256(binary.read_bytes()).digest() == hashlib.sha256(preserved.read_bytes()).digest()
        else:
            fixture.report['errors'].append('Runtime client changed outside this fixture; preserved original retained without overwriting it')
        fixture.report['interactive_preferences_unchanged'] = before == interactive_preferences(root)
        if not fixture.report['interactive_preferences_unchanged']:
            fixture.report['errors'].append('Inventory changed interactive preferences')
        fixture.report['passed'] &= not fixture.report['errors']
        (output / 'result.json').write_text(json.dumps(fixture.report, indent=2) + '\n')
    print(output, flush=True)
    if fixture.report['errors']:
        raise SystemExit('\n'.join(fixture.report['errors']))


if __name__ == '__main__':
    main()
