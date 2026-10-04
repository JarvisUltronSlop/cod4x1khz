"""Owned, isolated Vulkan loaded-world diagnostic; no FPS/replay acceptance claim."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import shutil
import time

from native_checkpoint_capture_probe import ManagedHostFixture
from native_fov_probe import interactive_preferences


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', type=int, default=28997)
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    output = root/'results'/('vulkan-world-'+datetime.now(timezone.utc).strftime('%Y%m%d-%H%M%S'))
    output.mkdir()
    runtime = root/'runtime/cod4x1khz.exe'
    source = root/'bin/Release/cod4x1khz.exe'
    saved = output/'preserved-runtime-client.exe'
    shutil.copy2(runtime, saved)
    tested_hash = hashlib.sha256(source.read_bytes()).hexdigest()
    preferences = interactive_preferences(root)
    saved_layer = os.environ.get('VK_LAYER_PATH')
    fixture = None
    report = {'scope': __doc__, 'passed': False, 'errors': [], 'tested_sha256': tested_hash,
              'full_world_replay_verified': False, 'timing_acceptance_verified': False}
    try:
        shutil.copy2(source, runtime)
        os.environ['VK_LAYER_PATH'] = str(root/'.deps/vulkan/layers/Release')
        fixture = ManagedHostFixture(root, output, args.port, budget=65,
            client_binary=runtime, client_settings={'r_graphicsApi': 'vulkan', 'r_vkValidation': 1, 'com_maxfps': 125})
        report = fixture.report
        report.update(scope=__doc__, full_world_replay_verified=False, timing_acceptance_verified=False)
        fixture.start()
        report['world_samples'] = [fixture.sample()]
        for _ in range(6):
            time.sleep(1)
            report['world_samples'].append(fixture.sample())
        report['passed'] = True
    except Exception as error:
        report['errors'].append(type(error).__name__+': '+str(error))
    finally:
        if fixture:
            fixture.close()
        if saved_layer is None:
            os.environ.pop('VK_LAYER_PATH', None)
        else:
            os.environ['VK_LAYER_PATH'] = saved_layer
        if hashlib.sha256(runtime.read_bytes()).hexdigest() == tested_hash:
            shutil.copy2(saved, runtime)
        report['runtime_restored'] = hashlib.sha256(runtime.read_bytes()).digest() == hashlib.sha256(saved.read_bytes()).digest()
        report['interactive_preferences_unchanged'] = interactive_preferences(root) == preferences
        report['passed'] &= report['runtime_restored'] and report['interactive_preferences_unchanged']
        (output/'result.json').write_text(json.dumps(report, indent=2)+'\n')
        print(json.dumps({'output': str(output), 'passed': report['passed'], 'errors': report['errors']}), flush=True)
    raise SystemExit(0 if report['passed'] else 1)


if __name__ == '__main__':
    main()
