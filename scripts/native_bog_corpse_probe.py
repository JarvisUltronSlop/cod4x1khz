"""Observe corpse bones and ragdolls in an isolated rendered bot match.

No desktop input, installed player profiles, or Internet hosting is involved.
The fixture owns its fresh client home and exact local server child.
"""
import argparse, copy, hashlib, json, os, subprocess, sys, time
from pathlib import Path

root = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(root / 'scripts'))
import native_checkpoint_capture_probe as host
from native_render_scene_probe import screenshot

parser = argparse.ArgumentParser()
parser.add_argument('--name', required=True)
parser.add_argument('--seconds', type=int, default=120)
parser.add_argument('--map', default='mp_bog')
parser.add_argument('--backend', choices=('vulkan', 'd3d9'), default='vulkan')
parser.add_argument('--port', type=int, default=29026)
parser.add_argument('--impacts', action='store_true', help='Fire a gold AK at loaded world geometry')
parser.add_argument('--stairs', action='store_true', help='Walk a shipped stair/ledge navigation pair')
parser.add_argument('--scoreboard', action='store_true')
parser.add_argument('--compare', action='store_true', help='Mirror the same immutable packets into private DX9 diagnostics')
parser.add_argument('--close-up', action='store_true', help='Move the protected native camera near the current corpse cluster')
args = parser.parse_args()
if args.seconds < 1 or args.seconds > 300: parser.error('--seconds must be between 1 and 300')
if args.compare:
    layers = root / '.deps/vulkan/layers/x64/Release'
    if not (layers / 'VkLayer_khronos_validation.json').is_file():
        parser.error('--compare requires the prepared x64 Vulkan validation layer')
    os.environ['VK_LAYER_PATH'] = str(layers)
out = root / 'results/bog-corpse-feedback' / args.name
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
f = Fixture(root, out, args.port, budget=args.seconds + 150, resolution='720p',
    client_binary=root/'runtime/x64/cod4x1khz.exe', client_settings={
        'r_graphicsApi': args.backend, 'r_vkCompare': int(args.compare), 'r_vkValidation': int(args.compare), 'snd_asyncStreams': 1, 'khz_renderPreset': 1,
        'r_vkFramesInFlight': 3, 'r_vkLegacySwapFence': 0, 'r_vkLegacySceneFence': 0,
        'r_vkOverlapUploads': 1, 'r_vkAsyncSubmit': 1, 'r_overlapFrontend': 1,
        'r_asyncSunQueries': 1, 'r_altModelLightingUpdate': 1})
f.server_binary = root/'runtime/x64/cod4x1khz_server.exe'
f.report['binary_sha256']['server'] = hashlib.sha256(f.server_binary.read_bytes()).hexdigest()
f.report['scope'] = f'Native {args.map} TDM with 15 bots, current-frame corpse skeletons, ragdoll bounds and rendered screenshots. No desktop automation or Internet testing.'
def capture_view(directory):
    shot = screenshot(f, directory)
    if args.compare:
        diagnostics = f.home/'vulkan/diagnostics'
        from native_screenshot_png import convert_bgra
        desc = json.loads((diagnostics/'reference.json').read_text())
        raw=[]
        for api in ('d3d9','vulkan'):
            pixels=(diagnostics/f'reference-{api}.bgra').read_bytes();raw.append(pixels)
            (directory/f'reference-{api}.png').write_bytes(convert_bgra(desc['width'],desc['height'],pixels))
        errors=[abs(a-b) for a,b in zip(*raw)]
        shot['comparison']={'same_immutable_packets':True,'max_channel_error':max(errors),
                            'mean_channel_error':sum(errors)/len(errors),'fraction_channels_error_gt8':sum(e>8 for e in errors)/len(errors)}
        (directory/'comparison.json').write_text(json.dumps(shot['comparison'],indent=2))
        if shot['comparison']['fraction_channels_error_gt8']>.02: raise ValueError('Vulkan diverged materially from the same DX9 draws')
    return shot

try:
    f.start()
    print(f'Native {args.map} match running', flush=True)
    f.report['lighting'] = f.call('khz_ui lighting')
    if f.report['lighting'].get('rainbow_missing_grid') or f.report['lighting'].get('show_grid'): raise ValueError('Shipping lighting enabled debug coloring')
    if args.scoreboard:
        f.report['scoreboard'] = checks = {'before': f.call('khz_ui scoreboard status')}
        checks['down'] = f.call('khz_ui scoreboard down')
        time.sleep(1)
        checks['held'] = f.call('khz_ui scoreboard status')
        shot = out/'scoreboard'; shot.mkdir()
        checks['screenshot'] = capture_view(shot)
        checks['up'] = f.call('khz_ui scoreboard up')
        time.sleep(.5)
        checks['released'] = f.call('khz_ui scoreboard status')
        if not checks['held'].get('shown') or checks['held'].get('scores') != 16 or checks['released'].get('shown'):
            raise ValueError('Real Tab press/release did not show sixteen live scores then hide the scoreboard')
    from native_render_scene_probe import choose, place, control, wait_view
    node, heading, _, scene = choose(f, root, args.map)
    f.report['scene'] = scene
    f.report['place'] = place(f, node['point'], heading['yaw'])
    f.report['input'] = control(f, f"fixed {heading['yaw']} 0")
    wait_view(f, node['point'], heading['yaw'])
    if args.close_up:
        import math
        models = f.call('khz_ui models')
        bodies = models.get('corpses', [])
        if not bodies: raise ValueError('No actual corpses available for close-up')
        target = min(bodies, key=lambda c: math.dist(c['origin'], node['point']))['origin']
        # Use shipped navigation positions and validate against loaded collision.
        rows=(root/'upstream/bot_warfare/scriptdata/waypoints'/f'{args.map}_wp.csv').read_text().splitlines()[1:]
        points=[list(map(float,row.split(',')[0].split())) for row in rows]
        from native_render_scene_probe import geometry, coordinates
        for point in sorted(points, key=lambda p: abs(math.dist(p[:2],target[:2])-120))[:10]:
            yaw=math.degrees(math.atan2(target[1]-point[1],target[0]-point[0]))
            if not geometry(f, 'probe '+coordinates([point])).get('clear'): continue
            # Avoid a wall between camera and corpse; use the rendered capture
            # as evidence, not just this navigation-distance heuristic.
            f.report['close_up']={'target':target,'position':point,'yaw':yaw}
            f.report['close_place']=place(f,point,yaw)
            pitch=math.degrees(math.atan2(point[2]+60-target[2],math.dist(point[:2],target[:2])))
            control(f,f'fixed {yaw} {min(80,pitch)}')
            break
        else: raise ValueError('No clear close-up position')
    if args.impacts:
        from native_render_scene_probe import geometry, coordinates
        f.report['weapon'] = geometry(f, f'weapon {f.port} ak47_mp 6')
        if 'error' in f.report['weapon']: raise ValueError(f.report['weapon']['error'])
        time.sleep(1)
        status = geometry(f, f'status {f.port}')
        f.report['fire'] = control(f, f"fire {status['angles'][1]} 25")
        time.sleep(3)
        control(f, f"fixed {status['angles'][1]} 25")
        # Let muzzle flashes, dust and impact lights settle before inspecting decals.
        time.sleep(1)
        impact_dir=out/'impacts';impact_dir.mkdir()
        f.report['impact_screenshot']=capture_view(impact_dir)
    if args.stairs:
        import math
        from native_render_scene_probe import geometry, coordinates
        rows=(root/'upstream/bot_warfare/scriptdata/waypoints'/f'{args.map}_wp.csv').read_text().splitlines()[1:]
        points=[list(map(float,row.split(',')[0].split())) for row in rows]
        pairs=[]
        for i,row in enumerate(rows):
            for link in map(int,row.split(',')[1].split()):
                a,b=points[i][:],points[link][:]
                if 12<abs(a[2]-b[2])<100 and 50<math.dist(a[:2],b[:2])<180:
                    pairs.append((a,b))
        for a,b in pairs[:30]:
            a[2]+=1;b[2]+=1
            if geometry(f,'probe '+coordinates([a])).get('clear') and geometry(f,'probe '+coordinates([b])).get('clear'):
                yaw=math.degrees(math.atan2(b[1]-a[1],b[0]-a[0]));place(f,a,yaw)
                look=[b[0]+800*math.cos(math.radians(yaw)),b[1]+800*math.sin(math.radians(yaw)),b[2]]
                f.report['stairs']={'start':a,'end':b,'look':look}
                control(f,'shuttle '+coordinates([a,b,look]));break
        else: raise ValueError('No loaded clear stair/ledge pair')
    samples = f.report['gameplay_samples'] = [f.sample()]
    f.report['first_server'] = f.native_server()
    until = time.monotonic() + args.seconds
    next_capture = time.monotonic()
    while time.monotonic() < until:
        time.sleep(min(2, max(0, until-time.monotonic())))
        samples.append(f.sample())
        models = f.call('khz_ui models')
        f.report.setdefault('model_samples', []).append(models)
        if time.monotonic() >= next_capture:
            capture = out / f'capture-{len(samples):03d}'; capture.mkdir()
            f.report.setdefault('screenshots', []).append(capture_view(capture))
            next_capture = time.monotonic() + 12
            print('Gameplay seconds', round(samples[-1]['elapsed'] - samples[0]['elapsed']), flush=True)
        if any(c['invalid'] or c['radius'] > 180 for c in models.get('corpses', [])): raise ValueError('Current rendered corpse bones exceeded finite human bounds')
        if any(c['invalid'] or c['radius'] > 180 for c in models.get('ragdolls', [])): raise ValueError('Ragdoll bones exceeded finite human bounds')
        if models.get('ragdolls'): print('Corpse radii', [(b['entity'], b['state'], b['capture_ms'], b['radius']) for b in models['ragdolls']], flush=True)
    f.report['last_server'] = f.native_server()
    if args.stairs:
        heights=[sample['state']['origin'][2] for sample in samples]
        f.report['stairs']['observed_height_range']=max(heights)-min(heights)
        if f.report['stairs']['observed_height_range']<10: raise ValueError('Native camera did not traverse the chosen stair/ledge')
    a,b=samples[0]['stats'],samples[-1]['stats']
    first,last=f.report['first_server'],f.report['last_server']
    f.report['rates']={'prediction_hz':(b['prediction_ticks']-a['prediction_ticks'])/(b['seconds']-a['seconds']),
                       'server_hz':(last['ticks']-first['ticks'])/(last['seconds']-first['seconds']),
                       'render_fps':(b['gpu_presents']-a['gpu_presents'])/(b['seconds']-a['seconds']),
                       'client_debt':b['debt'],'server_debt':last['debt'],
                       'snapshot_backlog':b['client_owner']['snapshot_backlog'],'event_backlog':b['client_owner']['event_backlog']}
    if args.seconds>=60 and not args.compare:
        if any(abs(f.report['rates'][key]-1000)>1 for key in ('prediction_hz','server_hz')):
            raise ValueError('Native 1 kHz simulation/prediction exceeded the 0.1% rate tolerance')
    f.leave()
    f.report['passed'] = True
except Exception as e:
    f.report['errors'].append(f'{type(e).__name__}: {e}')
    print('Fixture error', type(e).__name__, e, flush=True)
finally:
    f.close()
    (out/'report.json').write_text(json.dumps(f.report, indent=2), encoding='utf-8')
print('Passed', f.report['passed'], 'errors', f.report['errors'], flush=True)
raise SystemExit(0 if f.report['passed'] else 1)
