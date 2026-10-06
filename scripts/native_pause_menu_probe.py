"""Exercise rendered Esc menus and a bot match through native engine controls.

No desktop input, installed player profiles, or Internet hosting is involved.
The fixture owns its fresh client home and exact local server child.
"""
import argparse, copy, hashlib, json, subprocess, sys, time
from pathlib import Path

root = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(root / 'scripts'))
import native_checkpoint_capture_probe as host
from native_render_scene_probe import screenshot

parser = argparse.ArgumentParser()
parser.add_argument('--name', required=True)
parser.add_argument('--seconds', type=int, default=120)
parser.add_argument('--map', default='mp_overgrown')
parser.add_argument('--backend', choices=('vulkan', 'd3d9'), default='vulkan')
parser.add_argument('--port', type=int, default=29026)
args = parser.parse_args()
if args.seconds < 0 or args.seconds > 300: parser.error('--seconds must be between 0 and 300')
out = root / 'results/pause-overgrown-feedback' / args.name
out.mkdir(parents=True, exist_ok=False)
ready = host.ready_errors
def ready_map(sample, expected, client_pid, child_pid=None):
    shadow = copy.deepcopy(sample)
    actual_host = shadow.get('ui', {}).get('host', {})
    correct_mode = actual_host.get('gametype') == 'war'
    if actual_host.get('gametype') == 'war': actual_host['gametype'] = 'dm'
    errors = ready(shadow, args.map, client_pid, child_pid)
    if not correct_mode: errors.append('Owned server has not loaded the requested TDM mode')
    return errors
host.ready_errors = ready_map
original_popen = subprocess.Popen
def hidden_popen(*a, **kw):
    startup = subprocess.STARTUPINFO()
    startup.dwFlags = subprocess.STARTF_USESHOWWINDOW
    startup.wShowWindow = 0
    kw['startupinfo'] = startup
    return original_popen(*a, **kw)
subprocess.Popen = hidden_popen
class Fixture(host.ManagedHostFixture):
    def call(self, command, *a, **kw):
        if command == 'khz_ui host mp_killhouse dm 15 1':
            command = f'khz_ui host {args.map} war 15 1'
        return super().call(command, *a, **kw)
f = Fixture(root, out, args.port, budget=args.seconds + 90, resolution='720p',
    client_binary=root/'runtime/x64/cod4x1khz.exe', client_settings={
        'r_graphicsApi': args.backend, 'snd_asyncStreams': 1, 'khz_renderPreset': 1,
        'r_vkFramesInFlight': 3, 'r_vkLegacySwapFence': 0, 'r_vkLegacySceneFence': 0,
        'r_vkOverlapUploads': 1, 'r_vkAsyncSubmit': 1, 'r_overlapFrontend': 1,
        'r_asyncSunQueries': 1, 'r_altModelLightingUpdate': 1})
f.server_binary = root/'runtime/x64/cod4x1khz_server.exe'
f.report['binary_sha256']['server'] = hashlib.sha256(f.server_binary.read_bytes()).hexdigest()
f.report['scope'] = f'Native {args.map} TDM with 15 bots, actual Esc dispatch, rendered screenshot and native menu actions. No desktop automation or Internet testing.'
try:
    f.start()
    print('Native match running', flush=True)
    for press in range(2):
        f.call('khz_ui escape'); time.sleep(.3)
        state = f.call('khz_ui status')
        f.report.setdefault('escape_dispatches', []).append(state)
        if state['top_menu'] == 'khz_pause': break
    assert state['top_menu'] == 'khz_pause', state
    f.report['pause'] = f.call('khz_ui inspect khz_pause')
    assert f.report['pause']['visible']
    assert [i['text'] for i in f.report['pause']['items'][4:]] == ['Resume', 'Change Team', 'Change Class', 'Options', 'Leave Game', 'Quit']
    for item in f.report['pause']['items'][4:]:
        assert item['visible'] and item['enabled'] and item['text_alpha'] >= .9 and item['focus_alpha'] >= .9, item
        box, text = item['screen_rect'], item['text_rect']
        assert text[2] > 0 and text[3] > 0 and box[0] <= text[0] <= box[0] + box[2] - text[2], item
        assert box[1] + text[3] <= text[1] <= box[1] + box[3], item
        real = item['physical_text_rect']
        assert 0 <= real[0] and real[0] + real[2] <= 1280 and real[3] <= real[1] <= 720, item
    f.report['stock_team'] = f.call('khz_ui inspect team_marinesopfor')
    pause_dir = out/'pause'; pause_dir.mkdir()
    f.report['pause_screenshot'] = screenshot(f, pause_dir)
    print('Pause screenshot saved', flush=True)
    f.call('khz_ui escape')
    assert f.call('khz_ui status')['top_menu'] == ''
    f.call('khz_ui escape')
    assert f.call('khz_ui status')['top_menu'] == 'khz_pause'
    f.call('khz_ui key khz_pause 7 13'); time.sleep(.3)
    f.report['options'] = f.call('khz_ui status')
    assert f.report['options']['top_menu'] == 'main_options'
    options_dir = out/'options'; options_dir.mkdir()
    f.report['options_screenshot'] = screenshot(f, options_dir)
    f.call('khz_ui escape')
    if f.call('khz_ui status')['top_menu'] != 'khz_pause': f.call('khz_ui escape')
    assert f.call('khz_ui status')['top_menu'] == 'khz_pause'
    f.call('khz_ui key khz_pause 4 13')
    assert f.call('khz_ui status')['top_menu'] == ''
    samples = f.report['gameplay_samples'] = [f.sample()]
    f.report['first_server'] = f.native_server()
    until = time.monotonic() + args.seconds
    while time.monotonic() < until:
        time.sleep(min(2, max(0, until-time.monotonic())))
        samples.append(f.sample())
        if len(samples) % 15 == 0: print('Gameplay seconds', round(samples[-1]['elapsed'] - samples[0]['elapsed']), flush=True)
    f.report['last_server'] = f.native_server()
    # Exercise Leave through its actual menu confirmation, not the diagnostic
    # Leave command. Native fixture cleanup still handles any failed stage.
    f.call('khz_ui escape')
    assert f.call('khz_ui status')['top_menu'] == 'khz_pause'
    f.call('khz_ui key khz_pause 8 13')
    assert f.call('khz_ui status')['top_menu'] == 'popup_leavegame'
    confirm = f.report['leave_confirmation'] = f.call('khz_ui inspect popup_leavegame')
    yes = next(i for i in confirm['items'] if i['text'] == '@MENU_YES')
    f.call(f"khz_ui key popup_leavegame {yes['index']} 13")
    until = time.monotonic() + 15
    while time.monotonic() < until:
        try:
            after = f.sample()
            if host.leave_frontend_complete(after['stats'], after['state'], after['ui'], not f.server.alive()): break
        except (TimeoutError, OSError): pass
        time.sleep(.1)
    else: raise RuntimeError('Menu Leave did not stop the owned server and return to the frontend')
    f.report['explicit_leave_stopped_exact_server'] = True
    f.report['after_leave'] = after
    f.report['passed'] = True
except Exception as e:
    f.report['errors'].append(f'{type(e).__name__}: {e}')
    print('Fixture error', type(e).__name__, e, flush=True)
finally:
    f.close()
    (out/'report.json').write_text(json.dumps(f.report, indent=2), encoding='utf-8')
print('Passed', f.report['passed'], 'errors', f.report['errors'], flush=True)
raise SystemExit(0 if f.report['passed'] else 1)
