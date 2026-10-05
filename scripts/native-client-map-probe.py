"""Run owned x64 game processes and collect map integration evidence.

Broad-view bots join after camera protection so shader startup cannot kill the
fixture human. Measurements require both bots and a stable actual camera.
"""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import subprocess
import socket
import shutil
import time
from monitor import query
from gpu_metrics import GpuSampler
from native_fixture_ui import choose_assault

parser = argparse.ArgumentParser()
parser.add_argument("--server", default="build/x64/native-dedicated/native-server/Release/cod4x1khz_server_x64.exe")
parser.add_argument("--broad-view", action="store_true")
parser.add_argument("--client", default="build/x64/native-client/native-client/Release/cod4x1khz_x64.exe")
parser.add_argument("--renderer", choices=["d3d9", "vulkan"], default="d3d9")
parser.add_argument("--resolution", default="640x480")
parser.add_argument("--fps-cap", type=int, default=0)
parser.add_argument("--profile", action="store_true")
parser.add_argument("--cpu-profile", action="store_true", help="Capture eight seconds of native CPU spans and DX9 GPU timestamps")
parser.add_argument("--sample-cpu", action="store_true", help="Sample exact owned client thread stacks before the span capture; diagnostic only")
parser.add_argument("--compare", action="store_true")
parser.add_argument("--validation", action="store_true")
parser.add_argument("--seconds", type=int, default=40)
parser.add_argument("--extra-client-args", default="")
parser.add_argument("--output", default="results/x64-native-client-map")
args = parser.parse_args()
root = Path(__file__).resolve().parent.parent
probe = root / "build/x64/Release/Release/khz_launch_probe.exe"
server = (root / args.server).resolve()
client = (root / args.client).resolve()
prefix = root / args.output
prefix.parent.mkdir(parents=True, exist_ok=True)
env = dict(os.environ, KHZ_PROBE_HIDE_WINDOW="1", KHZ_PROBE_SECONDS="60")
env.pop("KHZ_WATCH_HUNK", None)
env.pop("KHZ_WATCH_SYMBOL", None)
env["KHZ_PROBE_SECONDS"] = str(max(60, args.seconds + 20))
if args.validation:
    env["VK_LAYER_PATH"] = str(root / ".deps/vulkan/layers/x64/Release")
server_args = "+set net_port 28997 +set dedicated 2 +set khz_benchmark 1 +set khz_test_controls 1 +set khz_bots 1 +set khz_bot_count " + ("0" if args.broad_view else "2") + " +set rcon_password local-khz-benchmark +set logfile 2 +set sv_lan 1 +map mp_backlot"
client_args = "+set net_port 28995 +set khz_benchmark 1 +set khz_test_controls 1 +set logfile 2 +set com_introPlayed 1 +set r_vsync 0 +set com_maxfps 0 +set r_graphicsApi " + args.renderer + " +connect 127.0.0.1:28997"
if not __import__("re").fullmatch(r"[0-9]{3,4}x[0-9]{3,4}", args.resolution) or not 0 <= args.fps_cap <= 10000 or not 20 <= args.seconds <= 300:
    parser.error("Invalid resolution or FPS cap")
client_args = client_args.replace("+set com_maxfps 0", "+set com_maxfps " + str(args.fps_cap))
client_args += " +set r_customMode " + args.resolution + " +set r_vkProfile " + ("1" if args.profile else "0")
client_args += " +set r_vkCompare " + ("1" if args.compare else "0") + " +set r_vkValidation " + ("1" if args.validation else "0")
client_args += " +set cl_timeout 300 +set cl_connectTimeout 300"
client_args += " +set r_altModelLightingUpdate 1 " + args.extra_client_args
samples = []
screenshot = None
shot_home = root / "runtime/benchmarks/client-28995/main/screenshots"
before_shots = set(shot_home.glob("shot*.*"))
shot_requested = False
shot_error = None
broad_view = None
selected_view = None
gpu_sampler = None
query_errors = {}
bots_requested = False
profile_requested = None
profile_paths = []
profile_home = root / "runtime/benchmarks/client-28995/vulkan/diagnostics"
profile_started = time.time()
cpu_profile = None
cpu_profile_path = None
cpu_profile_error = None
join_recovery = []
last_join_action = 0
cpu_sampler = None
cpu_sampling = None
join_inspections = []

def set_bots():
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.settimeout(1.2)
        sock.sendto(b"\xff" * 4 + b"rcon local-khz-benchmark set khz_bot_count 2", ("127.0.0.1", 28997))
        data, sender = sock.recvfrom(65536)
        if sender != ("127.0.0.1", 28997):
            raise ValueError("Bot command reply came from another endpoint")
        return data.decode("latin1")

class SceneFixture:
    port = 28995
    def rcon(self, command, marker="KHZ_JSON"):
        return query(28997, "rcon local-khz-benchmark " + command, marker, 1.2)
scene_fixture = SceneFixture()
with prefix.with_suffix(".server.log").open("w") as server_log, prefix.with_suffix(".client.log").open("w") as client_log:
    owned_server = subprocess.Popen([str(probe), str(server), str(root), str(server.parent), server_args], cwd=root, env=env, stdout=server_log, stderr=subprocess.STDOUT, creationflags=subprocess.CREATE_NO_WINDOW)
    time.sleep(6)
    if args.broad_view:
        from native_render_scene_probe import choose, place
        node, heading, nodes, evidence = choose(scene_fixture, root, "mp_backlot")
        selected_view = (node, heading, evidence)
    env["KHZ_PROBE_SECONDS"] = str(args.seconds)
    owned_client = subprocess.Popen([str(probe), str(client), str(root), str(client.parent), client_args], cwd=root, env=env, stdout=client_log, stderr=subprocess.STDOUT, creationflags=subprocess.CREATE_NO_WINDOW)
    deadline = time.monotonic() + args.seconds + 5
    while owned_client.poll() is None and time.monotonic() < deadline:
        try:
            state = query(28995, "khz_stats", "KHZ_JSON", .5)
            server_state = query(28997, "rcon local-khz-benchmark khz_json", "KHZ_JSON", .5)
            game_state = query(28995, "khz_state", "KHZ_STATE", .5)
            ui_state = query(28995, "khz_ui status", "KHZ_UI", .5)
            if gpu_sampler is None:
                gpu_sampler = GpuSampler([state["process_id"]])
            gpu_state = gpu_sampler.sample()
            samples.append({"time": time.time(), "client": state, "server": server_state, "state": game_state, "ui": ui_state, "gpu": gpu_state})
            # The automatic benchmark join can leave changeclass open after
            # spawning. Activate the actual stock assault control before camera
            # placement; never count a menu interval as a rendering benchmark.
            if (args.broad_view and broad_view is None and ui_state.get('top_menu') == 'changeclass' and
                    game_state.get('connection') == 9 and state['seconds'] > last_join_action + 2):
                last_join_action = state['seconds']
                first_inspection = [] if not join_inspections else None
                action = choose_assault(lambda text: query(28995, text, 'KHZ_UI', .5), ui_state, game_state, first_inspection)
                if first_inspection:
                    join_inspections = first_inspection
                    prefix.with_suffix('.join-menu.json').write_text(json.dumps(join_inspections, indent=2))
                if action:
                    join_recovery.append({'after_seconds': state['seconds'], **action})
            playing = state.get("prediction_ticks", 0) > 0 and game_state.get("pm_type") == 0 and (not ui_state.get("top_menu") or args.compare)
            if args.broad_view and playing and broad_view is None:
                node, heading, evidence = selected_view
                placed = place(scene_fixture, node["point"], heading["yaw"])
                fixed = query(28995, "khz_ui console khz_scene fixed " + str(heading["yaw"]) + " 0", "KHZ_UI", .5)
                broad_view = {"evidence": evidence, "placed": placed, "fixed": fixed, "after_presentations": state["presentations"], "after_seconds": state["seconds"]}
            if broad_view and not bots_requested:
                # RCON has a one-second throttle; placement itself uses RCON.
                time.sleep(1.08)
                broad_view["bots_command"] = set_bots()
                bots_requested = True
            if args.profile and profile_requested is None and broad_view and state["seconds"] > args.seconds - 12:
                profile_requested = {"after_seconds": state["seconds"], "reply": query(28995, "khz_ui console r_vkWriteProfile", "KHZ_UI", .5)}
            if args.sample_cpu and cpu_sampler is None and playing and broad_view and state["seconds"] > args.seconds - 26:
                sampling_path = prefix.parent / (prefix.name + '-cpu-stacks.log')
                cpu_sampling = {'after_seconds': state['seconds'], 'process_id': state['process_id'], 'path': str(sampling_path),
                                'scope': '8-second diagnostic wall-time stack samples and thread CPU deltas; temporarily suspends each sampled thread'}
                with sampling_path.open('w') as sampling_log:
                    cpu_sampler = subprocess.Popen([str(root / 'build/x64/Release/Release/khz_cpu_sample_probe.exe'),
                        str(state['process_id']), str(client), str(root / 'bin/x64/Release'), '8'],
                        cwd=root, stdout=sampling_log, stderr=subprocess.STDOUT, creationflags=subprocess.CREATE_NO_WINDOW)
            if args.cpu_profile and playing and broad_view and state["seconds"] > args.seconds - 13 and cpu_profile is None:
                cpu_profile = {"after_seconds": state["seconds"], "request": query(28995, "khz_ui renderprofile start 8000", "KHZ_UI", 1),
                               "statuses": [], "uploads_before": query(28995, "khz_ui renderprofile uploads", "KHZ_UI", 1)}
            if cpu_profile is not None and cpu_profile_path is None and cpu_profile_error is None:
                status = query(28995, "khz_ui renderprofile status", "KHZ_UI", 1)
                cpu_profile["statuses"].append(status)
                if status.get("phase") == 5:
                    cpu_profile_error = "Native CPU/GPU capture aborted"
                elif status.get("phase") == 4:
                    cpu_only = args.renderer == "vulkan" and not status.get("gpu_supported")
                    if (status.get("cpu_overflow") or status.get("query_errors") or status.get("gpu_skipped") or
                            (not cpu_only and (not status.get("gpu_supported") or status.get("disjoint") or
                            not status.get("gpu_frequency") or status.get("gpu_ready") != status.get("gpu_frames")))):
                        cpu_profile_error = "Native CPU/GPU capture is incomplete: " + json.dumps(status)
                    else:
                        cpu_profile["scope"] = "CPU spans only; use the separate serial-matched Vulkan GPU timing export" if cpu_only else "CPU spans and DX9 GPU timestamps"
                        cpu_profile["export"] = query(28995, "khz_ui renderprofile export", "KHZ_UI", 4)
                        path = root / "runtime/benchmarks/client-28995" / ("render-profile-" + str(status["start_qpc"]) + ".json")
                        data = path.read_bytes()
                        cpu_profile["exported_after_seconds"] = state["seconds"]
                        capture = json.loads(data)
                        if len(capture["cpu"]) != status["cpu_main"] + status["cpu_backend"] or len(capture["gpu"]) != status["gpu_frames"]:
                            cpu_profile_error = "Native exported capture differs from frozen counters"
                        else:
                            cpu_profile_path = str(prefix.parent / (prefix.name + "-cpu-profile.json"))
                            Path(cpu_profile_path).write_bytes(data)
                            cpu_profile["capture_sha256"] = hashlib.sha256(data).hexdigest()
                            cpu_profile["uploads_after"] = query(28995, "khz_ui renderprofile uploads", "KHZ_UI", 1)
            if playing and (not args.broad_view or broad_view is not None and state["presentations"] - broad_view["after_presentations"] > 300 and state["seconds"] >= args.seconds - 14) and not shot_requested:
                reply = query(28995, "khz_ui screenshot", "KHZ_UI", .5)
                shot_requested = True
                if reply.get("result") != "queued": shot_error = str(reply)
            if shot_requested and screenshot is None:
                for path in sorted(set(shot_home.glob("shot*.*")) - before_shots):
                    data = path.read_bytes()
                    if len(data) < 1000: continue
                    from native_screenshot_png import convert
                    suffix = ".png" if path.suffix.lower() == ".tga" else path.suffix
                    screenshot = str(prefix.with_suffix(suffix))
                    Path(screenshot).write_bytes(convert(data) if suffix == ".png" else data)
                    break
        except (OSError, TimeoutError, ValueError, RuntimeError) as error:
            message = str(error)
            query_errors[message] = query_errors.get(message, 0) + 1
        time.sleep(.5)
    client_exit = owned_client.wait(timeout=15)
    # The probe is the owning debugger for its exact created child. A client
    # failure must not leave the server waiting out a long benchmark lifetime.
    if client_exit and owned_server.poll() is None:
        owned_server.terminate()
    try:
        server_exit = owned_server.wait(timeout=60)
    except subprocess.TimeoutExpired:
        owned_server.terminate()
        server_exit = owned_server.wait(timeout=15)
if cpu_sampler is not None:
    cpu_sampling['exit_code'] = cpu_sampler.wait(timeout=15)
if gpu_sampler:
    gpu_sampler.close()
for path in profile_home.glob("*"):
    if path.is_file() and path.stat().st_mtime >= profile_started and (path.name == "frame-timings.csv" or args.compare and path.name.startswith("reference")):
        destination = prefix.parent / (prefix.name + "-diagnostics")
        destination.mkdir(exist_ok=True)
        shutil.copy2(path, destination / path.name)
        profile_paths.append(str(destination / path.name))
prefix.with_suffix(".json").write_text(json.dumps({"client_exit": client_exit, "server_exit": server_exit, "renderer": args.renderer, "resolution": args.resolution, "fps_cap": args.fps_cap, "profile": args.profile, "profile_export": profile_requested, "diagnostic_paths": profile_paths, "cpu_profile": cpu_profile, "cpu_profile_path": cpu_profile_path, "cpu_profile_error": cpu_profile_error, "compare": args.compare, "validation": args.validation, "samples": samples, "query_errors": query_errors, "screenshot": screenshot, "screenshot_error": shot_error, "broad_view": broad_view,
    "binary_sha256": {str(p.relative_to(root)): hashlib.sha256(p.read_bytes()).hexdigest() for p in (client, server)}}, indent=2))
report = json.loads(prefix.with_suffix(".json").read_text())
report['join_recovery'] = join_recovery
report['cpu_sampling'] = cpu_sampling
measurement = []
camera_errors = []
if broad_view:
    point = broad_view["evidence"]["selected"]["point"]
    yaw = broad_view["evidence"]["heading"]["yaw"]
    for sample in samples:
        if (sample["client"]["seconds"] < broad_view["after_seconds"] + 3 or sample["server"].get("native_bots", {}).get("active", 0) != 2 or
                profile_requested and sample["client"]["seconds"] >= profile_requested["after_seconds"] or
                cpu_profile and sample["client"]["seconds"] >= cpu_profile["after_seconds"] or
                cpu_sampling and sample["client"]["seconds"] >= cpu_sampling["after_seconds"]):
            continue
        view = sample["state"].get("rendered_view_angles", [])
        origin = sample["state"].get("origin", [])
        if (sample["state"].get("pm_type") != 0 or sample["ui"].get("top_menu") or len(view) != 3 or len(origin) != 3 or
                math.dist(origin[:2], point[:2]) > 20 or abs((view[1] - yaw + 180) % 360 - 180) > 2 or abs((view[0] + 180) % 360 - 180) > 2):
            camera_errors.append({"time": sample["time"], "state": sample["state"], "ui": sample["ui"]})
        measurement.append(sample)
if len(measurement) >= 2:
    first, last = measurement[0], measurement[-1]
    duration = last["client"]["seconds"] - first["client"]["seconds"]
    server_duration = last["server"]["seconds"] - first["server"]["seconds"]
    report["measurement"] = {"seconds": duration, "samples": len(measurement), "camera_passed": not camera_errors,
        "camera_errors": camera_errors,
        "fps": (last["client"]["gpu_presents"] - first["client"]["gpu_presents"]) / duration,
        "prediction_hz": (last["client"]["prediction_ticks"] - first["client"]["prediction_ticks"]) / duration,
        "server_hz": (last["server"]["ticks"] - first["server"]["ticks"]) / server_duration,
        "client_debt": last["client"]["debt"], "server_debt": last["server"]["debt"]}
else:
    report["measurement"] = {"camera_passed": False, "error": "No sustained live broad-view interval was captured"}
if cpu_profile and broad_view:
    profile_samples = [sample for sample in samples if
        cpu_profile['after_seconds'] <= sample['client']['seconds'] <= cpu_profile.get('exported_after_seconds', -1)]
    profile_camera_errors = []
    for sample in profile_samples:
        view = sample['state'].get('rendered_view_angles', [])
        origin = sample['state'].get('origin', [])
        if (sample['state'].get('pm_type') != 0 or sample['ui'].get('top_menu') or len(view) != 3 or len(origin) != 3 or
                math.dist(origin[:2], point[:2]) > 20 or abs((view[1] - yaw + 180) % 360 - 180) > 2 or abs((view[0] + 180) % 360 - 180) > 2 or
                sample['server'].get('native_bots', {}).get('active', 0) != 2):
            profile_camera_errors.append({'time': sample['time'], 'state': sample['state'], 'ui': sample['ui']})
    report['cpu_profile_camera'] = {'passed': len(profile_samples) >= 10 and not profile_camera_errors,
        'samples': len(profile_samples), 'errors': profile_camera_errors}
    if not report['cpu_profile_camera']['passed']:
        report['cpu_profile_error'] = cpu_profile_error = 'Camera or bot fixture changed during CPU/GPU capture'
prefix.with_suffix(".json").write_text(json.dumps(report, indent=2))
print(json.dumps(report["measurement"], indent=2), flush=True)
if query_errors:
    print(json.dumps(query_errors, indent=2), flush=True)
print("Client exit", client_exit, "server exit", server_exit, "telemetry samples", len(samples), flush=True)
print(prefix.with_suffix(".client.log").read_text(errors="replace")[-6500:], flush=True)
if args.cpu_profile and (not cpu_profile_path or cpu_profile_error):
    print("CPU profile failed:", cpu_profile_error or "No complete capture exported", flush=True)
sampling_failed = args.sample_cpu and (cpu_sampling is None or cpu_sampling.get('exit_code') != 0)
if sampling_failed:
    print("CPU sampling failed: no successful exact-process sample was retained", flush=True)
raise SystemExit(client_exit or server_exit or (args.broad_view and not report["measurement"]["camera_passed"]) or (args.cpu_profile and (not cpu_profile_path or cpu_profile_error)) or sampling_failed)
