"""Alternating actual upload paths and an FPS cap in one owned native match.

Eight-second stages are a throughput diagnostic, not identical-world replay.
GPU/CPU observers stay outside the game. No GPU timestamp profiling in stages.
"""
from datetime import datetime, timezone
from pathlib import Path
from types import SimpleNamespace
import argparse
import hashlib
import json
import subprocess
import sys
import time
from native_checkpoint_capture_probe import ManagedHostFixture
from native_fov_probe import interactive_preferences
from native_render_cap_probe import ManagedCapServer, admission_checks, bot_control_rates
from render_cap_probe import run_stage


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--build', type=int, default=86)
    p.add_argument('--resolution', choices=('480p', '720p'), default='480p')
    p.add_argument('--client-port', type=int, default=28994)
    a = p.parse_args()
    root = Path(__file__).resolve().parent.parent
    archive = root / f'results/build{a.build}-source-and-binary-hashes.json'
    record = json.loads(archive.read_text(encoding='utf-8-sig'))
    names = ('cod4x1khz.exe', 'runtime/cod4x1khz_server.exe')
    if any(hashlib.sha256((root / n).read_bytes()).hexdigest() != record['binary_sha256'][n] for n in names):
        raise ValueError('Native binaries differ from selected build archive')
    out = root / 'results' / (f'native-render-upload{a.build}-' + datetime.now(timezone.utc).strftime('%Y%m%d-%H%M%S-%f'))
    out.mkdir()
    f = ManagedHostFixture(root, out, a.client_port, budget=140, resolution=a.resolution)
    prefs = interactive_preferences(root)
    observer = None
    try:
        f.start()
        # This lightweight observer is independent of diagnostic queries; its
        # startup and hashes occur before the first measured eight-second stage.
        observer = subprocess.Popen([sys.executable, str(root / 'scripts/render_usage_observer.py'),
            '--client', str(f.client.pid), '--server', str(f.server.pid), '--output', str(out / 'usage.json'),
            '--seconds', '65'])
        server = ManagedCapServer(f)
        args = SimpleNamespace(client_port=f.port, map='mp_killhouse', resolution=a.resolution, desired_bots=15)
        log = f.server_home / 'main/khz_server.log'
        initial_errors = log.read_text(encoding='latin1').count('******* script runtime error *******')
        f.report['scope'] = ('Same actual match: alternate legacy/staged uploads, then cap 1000 FPS; 2 s warmup and 8 s samples each. '
            'Continuous input, bots and world evolution; no identical-input/full-world replay or sustained acceptance claim.')
        stages = f.report['stages'] = []
        for mode, cap in ((0, 0), (1, 0), (0, 0), (1, 0), (1, 1000)):
            applied = f.call('khz_ui renderprofile uploadmode ' + str(mode))
            if applied.get('enabled') != bool(mode) or applied.get('failures'):
                raise ValueError('Actual upload mode was rejected or ownership failed')
            time.sleep(1) # Let the already submitted previous-mode frame finish.
            before = f.call('khz_ui renderprofile uploads')
            started = datetime.now(timezone.utc).isoformat()
            stage = run_stage(cap, args, server)
            stage.update(upload_mode=mode, started_utc=started, finished_utc=datetime.now(timezone.utc).isoformat(), uploads_before=before)
            after = stage['uploads_after'] = f.call('khz_ui renderprofile uploads')
            stage['admission'] = admission_checks(stage, f, a.resolution)
            stage['bot_controls'] = bot_control_rates(stage)
            stage['uploads_pass'] = (after.get('enabled') == bool(mode) and after.get('failures') == 0 and
                (after['upload_frames'] > before['upload_frames'] and after['staged_locks'] > before['staged_locks'] if mode else
                 all(after[k] == before[k] for k in ('upload_frames', 'staged_locks', 'upload_bytes'))))
            stage['passed'] &= stage['admission']['passed'] and stage['bot_controls']['passed'] and stage['uploads_pass']
            stages.append(stage)
            print(f"Upload {mode}, cap {cap or 'uncapped'}: {stage['client']['gpu_presents_hz']:.1f} FPS; server p99 {stage['work_p99_us']} us; passed={stage['passed']}", flush=True)
        f.report['new_script_errors'] = max(0, log.read_text(encoding='latin1').count('******* script runtime error *******') - initial_errors)
        f.report['passed'] = all(s['passed'] for s in stages) and not f.report['new_script_errors']
        if not f.report['passed']:
            f.report['errors'].append('An actual upload/cap stage or script check failed')
        f.leave()
    except Exception as e:
        f.report['passed'] = False
        f.report['errors'].append(str(e))
    finally:
        f.close()
        if observer:
            try: observer.wait(timeout=15)
            except subprocess.TimeoutExpired:
                observer.terminate(); observer.wait(timeout=5)
                f.report['errors'].append('Exact usage observer exceeded its bounded run')
        f.report['interactive_preferences_unchanged'] = prefs == interactive_preferences(root)
        f.report['binary_provenance_pass'] = all(hashlib.sha256((root / n).read_bytes()).hexdigest() == record['binary_sha256'][n] for n in names)
        f.report['passed'] &= (not f.report['errors'] and f.report['interactive_preferences_unchanged'] and f.report['binary_provenance_pass'] and
            f.report['cleanup']['created_client_returncode'] == 0 and f.report['cleanup']['exact_owned_server_exited'])
        (out / 'result.json').write_text(json.dumps(f.report, indent=2) + '\n')
    print(json.dumps({'passed': f.report['passed'], 'errors': f.report['errors'], 'output': str(out)}, indent=2))
    return 0 if f.report['passed'] else 1

if __name__ == '__main__':
    raise SystemExit(main())
