"""Exercise stock audio slider mouse capture across actual engine paint frames.

Uses isolated native fixture homes and exact owned local servers. No desktop
automation, installed player profiles or Internet hosting is involved.
"""
import argparse
import hashlib
import json
import subprocess
import sys
import time
from pathlib import Path

root = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(root / 'scripts'))
from native_checkpoint_capture_probe import ManagedHostFixture
from native_fov_probe import stock_inventory, stock_action, one_control, archived_config, archive_evidence
from native_render_scene_probe import screenshot

parser = argparse.ArgumentParser()
parser.add_argument('--name', required=True)
parser.add_argument('--backend', choices=('vulkan', 'd3d9'), default='vulkan')
parser.add_argument('--port', type=int, required=True)
parser.add_argument('--seconds', type=int, default=60)
args = parser.parse_args()
if not 0 <= args.seconds <= 180: parser.error('--seconds must be between 0 and 180')
out = root / 'results/volume-slider-feedback' / args.name
out.mkdir(parents=True, exist_ok=False)
original_popen = subprocess.Popen
def hidden_popen(*a, **kw):
    startup = subprocess.STARTUPINFO()
    startup.dwFlags = subprocess.STARTF_USESHOWWINDOW
    startup.wShowWindow = 0
    kw['startupinfo'] = startup
    return original_popen(*a, **kw)
subprocess.Popen = hidden_popen
fixture = ManagedHostFixture(root, out, args.port, budget=args.seconds + 120, resolution='720p',
    client_binary=root/'runtime/x64/cod4x1khz.exe', client_settings={
        'r_graphicsApi': args.backend, 'snd_asyncStreams': 1, 'khz_renderPreset': 1,
        'r_vkFramesInFlight': 3, 'r_vkLegacySwapFence': 0, 'r_vkLegacySceneFence': 0,
        'r_vkOverlapUploads': 1, 'r_vkAsyncSubmit': 1, 'r_overlapFrontend': 1,
        'r_asyncSunQueries': 1, 'r_altModelLightingUpdate': 1})
fixture.server_binary = root/'runtime/x64/cod4x1khz_server.exe'
fixture.report['binary_sha256']['server'] = hashlib.sha256(fixture.server_binary.read_bytes()).hexdigest()
fixture.report['scope'] = 'Stock volume slider drag, click and archive in a live 15-bot match and disconnected frontend, native UI dispatch and paint. No desktop automation or Internet.'

def open_sound(context):
    items = stock_inventory(fixture, 'main_options')
    # Every category's script names all submenus while hiding the others.
    # Select the actual Sound button rather than matching a hidden-menu name.
    button = one_control(items, text='@MENU_SOUND', kind=1)
    context['navigation'] = stock_action(fixture, 'main_options', button)
    assert fixture.call('khz_ui status')['top_menu'] == 'options_sound'
    slider = one_control(stock_inventory(fixture, 'options_sound'), dvar='snd_volume', kind=10, require_action=False)
    context['slider'] = slider
    return slider['index']

def volume_case(label):
    case = {'name': label, 'passed': False, 'events': []}
    fixture.report.setdefault('volume_cases', []).append(case)
    index = open_sound(case)
    command = f'khz_ui slider options_sound {index}'
    def event(phase, fraction=None):
        reply = fixture.call(command + ' ' + phase + ('' if fraction is None else ' ' + str(fraction)))
        assert reply.get('result') == 'slider event dispatched', reply
        case['events'].append({'phase': phase, 'fraction': fraction, 'reply': reply})
        return reply
    initial = event('status')
    def expected(fraction):
        return initial['min'] + (initial['max'] - initial['min']) * min(1, max(0, fraction))
    def wait_value(fraction, captured):
        until = time.monotonic() + 2
        while time.monotonic() < until:
            reply = event('status')
            assert reply['captured'] is captured, reply
            if abs(reply['value'] - expected(fraction)) <= .015 * (initial['max'] - initial['min']): return reply
            time.sleep(.02)
        raise TimeoutError(f'Slider did not reach fraction {fraction}')
    assert event('begin')['captured']
    for fraction in [.8, .6, .4, .2, 0, .5, 1, 1.2, -.2, .65]:
        assert event('move', fraction)['captured']
        case.setdefault('painted_values', []).append(wait_value(fraction, True))
        time.sleep(.06)  # Keep capture alive across multiple actual Menu_PaintAll calls.
    released = event('end')
    assert not released['captured']
    event('move', .25); time.sleep(.12)
    assert abs(event('status')['value'] - released['value']) < .0001
    clicked = event('click', .3)
    assert not clicked['captured']
    wait_value(.3, False)
    capture_dir = out/label; capture_dir.mkdir()
    case['screenshot'] = screenshot(fixture, capture_dir)
    until = time.monotonic() + 3
    while time.monotonic() < until:
        config = archived_config(fixture)
        if abs(float(config['settings'].get('snd_volume', '-99')) - expected(.3)) < .015: break
        time.sleep(.1)
    else: raise RuntimeError('Changed volume was not archived')
    case['archive'] = archive_evidence(config, ('snd_volume',))
    case['passed'] = True
    print(label, 'volume drag/click/archive passed', flush=True)

try:
    fixture.start()
    for _ in range(2):
        fixture.call('khz_ui escape')
        if fixture.call('khz_ui status')['top_menu'] == 'khz_pause': break
    assert fixture.call('khz_ui status')['top_menu'] == 'khz_pause'
    fixture.call('khz_ui key khz_pause 7 13')
    assert fixture.call('khz_ui status')['top_menu'] == 'main_options'
    volume_case('in-game')
    back = one_control(stock_inventory(fixture, 'options_sound'), name='back', kind=1)
    stock_action(fixture, 'options_sound', back)
    fixture.call('khz_ui escape')
    if fixture.call('khz_ui status')['top_menu'] != 'khz_pause': fixture.call('khz_ui escape')
    assert fixture.call('khz_ui status')['top_menu'] == 'khz_pause'
    fixture.call('khz_ui key khz_pause 4 13')
    samples = fixture.report['gameplay_samples'] = [fixture.sample()]
    fixture.report['first_server'] = fixture.native_server()
    until = time.monotonic() + args.seconds
    while time.monotonic() < until:
        time.sleep(min(2, max(0, until-time.monotonic())))
        samples.append(fixture.sample())
    fixture.report['last_server'] = fixture.native_server()
    fixture.leave()
    opened = fixture.call('khz_ui menu main_options')
    assert opened['top_menu'] == 'main_options'
    volume_case('frontend')
    fixture.report['passed'] = True
except Exception as error:
    fixture.report['errors'].append(f'{type(error).__name__}: {error}')
    print('Fixture error:', type(error).__name__, error, flush=True)
finally:
    fixture.close()
    (out/'report.json').write_text(json.dumps(fixture.report, indent=2)+'\n', encoding='utf-8')
print('Passed', fixture.report['passed'], 'errors', fixture.report['errors'], flush=True)
raise SystemExit(0 if fixture.report['passed'] else 1)
