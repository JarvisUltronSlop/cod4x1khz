"""Read actual engine telemetry over localhost; no simulated work is substituted."""
import argparse
import ctypes as ct
from ctypes import wintypes as wt
import json
import hashlib
import os
from pathlib import Path
import socket
import time
from gpu_metrics import GpuSampler


def query(port, command, marker="KHZ_JSON", timeout=1.2):
    if timeout <= 0:
        raise ValueError("Query timeout must be positive")
    until = time.monotonic() + timeout
    marker_bytes = (marker + " ").encode("ascii")
    accumulated = bytearray()
    decoder = json.JSONDecoder()
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.settimeout(timeout)
        sock.sendto(b"\xff" * 4 + command.encode("ascii"), ("127.0.0.1", port))
        while True:
            remaining = until - time.monotonic()
            if remaining <= 0:
                break
            sock.settimeout(remaining)
            try:
                data, sender = sock.recvfrom(65536)
            except socket.timeout:
                break
            if sender != ("127.0.0.1", port):
                continue
            # SV_FlushRedirect splits its output into independent print packets.
            # Strip each packet's framing before joining the diagnostic stream.
            if data.startswith(b"\xff" * 4):
                data = data[4:]
            if data.startswith(b"print\n"):
                data = data[6:]
            if len(accumulated) + len(data) > 65536:
                raise ValueError(f"{marker} reply from {port} exceeds 65536 bytes")
            accumulated.extend(data)
            marker_start = accumulated.find(marker_bytes)
            if marker_start < 0:
                continue
            text = accumulated[marker_start + len(marker_bytes):].decode("latin1")
            try:
                return decoder.raw_decode(text)[0]
            except json.JSONDecodeError:
                # Both the marker and JSON body may cross packet boundaries.
                continue
    raise TimeoutError(f"No complete {marker} reply from {port} within {timeout:g} seconds")


def independent_owner_path(samples):
    """Require the same independent prediction owner throughout a live stage."""
    owners = [sample.get('client_owner', {}) for sample in samples]
    if len(owners) < 2:
        return {'passed': False, 'reason': 'At least two owner samples are required'}
    initial, final = owners[0], owners[-1]
    ticks = final.get('ticks', 0) - initial.get('ticks', 0)
    poses = final.get('pose_ticks', 0) - initial.get('pose_ticks', 0)
    stable = all(owner.get('active') and owner.get('private_domains')
                 and owner.get('generation') == initial.get('generation')
                 and owner.get('session') == initial.get('session')
                 and owner.get('queue_failures') == 0 for owner in owners)
    return {'generation': initial.get('generation'), 'session': initial.get('session'),
            'ticks': ticks, 'pose_ticks': poses, 'passed': bool(stable and ticks > 0 and poses > 0)}


def collision_path(first, last, tolerance_hz=1):
    """Require complete snapshot-derived prediction without legacy fallback."""
    fields = ('prediction_ticks', 'snapshot_collision_ticks', 'legacy_collision_ticks')
    missing = [f'{which}.{field}' for which, sample in (('first', first), ('last', last))
               for field in fields if field not in sample]
    result = {'collision_available': not missing, 'missing_collision_fields': missing,
              'snapshot_collision_ticks': None, 'legacy_collision_ticks': None,
              'snapshot_collision_ticks_hz': None, 'legacy_collision_ticks_hz': None,
              'snapshot_collision_fraction': None, 'collision_pass': False,
              'collision_reason': ''}
    if missing:
        result['collision_reason'] = 'Collision path telemetry is missing; this build cannot establish snapshot collision coverage'
        return result
    if any(type(sample[field]) is not int or sample[field] < 0
           for sample in (first, last) for field in fields):
        result['collision_reason'] = 'Collision or prediction counters are invalid'
        return result
    duration = last['seconds'] - first['seconds']
    delta = {field: last[field] - first[field] for field in fields}
    result.update({field: delta[field] for field in fields[1:]})
    if duration <= 0 or any(value < 0 for value in delta.values()):
        result['collision_reason'] = 'Telemetry clock or collision counters reset during the measurement'
        return result
    result.update({field + '_hz': delta[field] / duration for field in fields[1:]})
    if delta['prediction_ticks'] > 0:
        result['snapshot_collision_fraction'] = delta['snapshot_collision_ticks'] / delta['prediction_ticks']
    reasons = []
    if delta['prediction_ticks'] <= 0:
        reasons.append('No prediction ticks were measured')
    if delta['snapshot_collision_ticks'] != delta['prediction_ticks']:
        reasons.append('Snapshot collision did not cover every prediction tick')
    if delta['legacy_collision_ticks'] != 0:
        reasons.append(f"Legacy collision fallback used on {delta['legacy_collision_ticks']} ticks")
    if abs(result['snapshot_collision_ticks_hz'] - 1000) > tolerance_hz:
        reasons.append(f'Snapshot collision rate differs from 1000 Hz by more than {tolerance_hz} Hz')
    result['collision_pass'] = not reasons
    result['collision_reason'] = '; '.join(reasons)
    return result


class MemoryCounters(ct.Structure):
    _fields_ = [("cb", wt.DWORD), ("faults", wt.DWORD)] + [
        (name, ct.c_size_t) for name in (
            "peak_rss", "rss", "peak_paged", "paged", "peak_nonpaged",
            "nonpaged", "pagefile", "peak_pagefile", "private")]


class ProcessSampler:
    def __init__(self, pids):
        self.kernel = ct.WinDLL("kernel32", use_last_error=True)
        self.psapi = ct.WinDLL("psapi", use_last_error=True)
        self.kernel.OpenProcess.argtypes = [wt.DWORD, wt.BOOL, wt.DWORD]
        self.kernel.OpenProcess.restype = wt.HANDLE
        self.kernel.CloseHandle.argtypes = [wt.HANDLE]
        self.kernel.GetProcessTimes.argtypes = [wt.HANDLE] + [ct.POINTER(wt.FILETIME)] * 4
        self.kernel.OpenThread.argtypes = [wt.DWORD, wt.BOOL, wt.DWORD]
        self.kernel.OpenThread.restype = wt.HANDLE
        self.kernel.GetThreadTimes.argtypes = [wt.HANDLE] + [ct.POINTER(wt.FILETIME)] * 4
        self.threads = {}
        self.psapi.GetProcessMemoryInfo.argtypes = [wt.HANDLE, ct.POINTER(MemoryCounters), wt.DWORD]
        self.handles = {pid: self.kernel.OpenProcess(0x410, False, pid) for pid in pids}
        if any(not handle for handle in self.handles.values()):
            self.close()
            raise OSError("A benchmark process could not be inspected")

    def sample(self):
        result = {}
        for pid, handle in self.handles.items():
            times = [wt.FILETIME() for _ in range(4)]
            memory = MemoryCounters()
            memory.cb = ct.sizeof(memory)
            if not self.kernel.GetProcessTimes(handle, *(ct.byref(t) for t in times)):
                raise OSError(f"Could not read process {pid}")
            if not self.psapi.GetProcessMemoryInfo(handle, ct.byref(memory), memory.cb):
                raise OSError(f"Could not read memory for {pid}")
            seconds = sum((t.dwHighDateTime << 32) | t.dwLowDateTime for t in times[2:]) / 1e7
            result[str(pid)] = {"cpu_seconds": seconds, "rss_bytes": memory.rss,
                                "private_bytes": memory.private}
        return result

    def close(self):
        for handle in self.threads.values():
            if handle:
                self.kernel.CloseHandle(handle)
        self.threads = {}
        for handle in self.handles.values():
            if handle:
                self.kernel.CloseHandle(handle)
        self.handles = {}

    def sample_threads(self, telemetry):
        result = {}
        for process in telemetry:
            for role in ('main', 'render'):
                tid = process.get(role + '_thread_id', 0)
                if not tid:
                    continue
                if tid not in self.threads:
                    self.threads[tid] = self.kernel.OpenThread(0x800, False, tid)
                handle = self.threads[tid]
                times = [wt.FILETIME() for _ in range(4)]
                if handle and self.kernel.GetThreadTimes(handle, *(ct.byref(t) for t in times)):
                    seconds = sum((t.dwHighDateTime << 32) | t.dwLowDateTime for t in times[2:]) / 1e7
                    result[str(tid)] = {'process_id': process['process_id'], 'role': role, 'cpu_seconds': seconds}
        return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--seconds", type=float, default=60)
    parser.add_argument("--server-port", type=int, default=28960)
    parser.add_argument("--client-ports", type=int, nargs="+", default=[28962])
    parser.add_argument("--pids", type=int, nargs="*", default=[])
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--map", default="mp_killhouse")
    parser.add_argument("--render-fps", type=int, default=0,
                        help="Presentation cap, 0 for uncapped rendering (gameplay stays at 1,000 Hz)")
    parser.add_argument("--resolution", default="480p")
    parser.add_argument("--desired-bots", type=int, default=15)
    args = parser.parse_args()
    if not 0 <= args.render_fps <= 10000:
        parser.error("--render-fps must be between 0 (uncapped) and 10000")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    sampler = ProcessSampler(args.pids)
    gpu = GpuSampler(args.pids)
    report = {"map": args.map, "requested_seconds": args.seconds,
              "render_cap": args.render_fps, "resolution": args.resolution,
              "logical_cpus": os.cpu_count(), "samples": [], "errors": [],
              "scope": "live match telemetry; movement coverage and render independence require separate tests"}
    root = Path(__file__).resolve().parent.parent
    report['binary_sha256'] = {name: hashlib.sha256((root / 'runtime' / name).read_bytes()).hexdigest()
                               for name in ('cod4x1khz.exe', 'cod4x1khz_server.exe')}
    report['source_pins'] = json.loads((root / 'upstream-lock.json').read_text(encoding='utf8'))

    def read_sample():
        server = query(args.server_port, "rcon local-khz-benchmark khz_json")
        clients = {str(p): query(p, "khz_stats") for p in args.client_ports}
        return {"elapsed": time.monotonic() - start, "server": server,
                "clients": clients, "processes": sampler.sample(),
                "threads": sampler.sample_threads([server, *clients.values()]), "gpu": gpu.sample()}

    start = time.monotonic()
    next_read = start
    try:
        while True:
            time.sleep(max(0, next_read - time.monotonic()))
            try:
                report["samples"].append(read_sample())
            except (OSError, ValueError) as error:
                report["errors"].append({"elapsed": time.monotonic() - start, "error": str(error)})
                if len(report["errors"]) > 4:
                    break
            elapsed = time.monotonic() - start
            if int(elapsed) % 30 == 0 and report["samples"]:
                sample = report["samples"][-1]
                print(f"{elapsed:.1f}s: server p99 {sample['server']['work_p99_us']} us, debt {sample['server']['debt']}", flush=True)
            if elapsed >= args.seconds:
                break
            next_read = min(start + args.seconds, next_read + 2)
    finally:
        sampler.close()
        gpu.close()
        samples = report["samples"]
        if len(samples) > 1:
            first, last = samples[0], samples[-1]
            def rates(a, b, names):
                duration = b["seconds"] - a["seconds"]
                return {name + "_hz": (b[name] - a[name]) / duration for name in names}
            server = rates(first["server"], last["server"], ["ticks", "commands_received", "snapshots_sent"])
            if 'snapshot_bytes' in first['server']:
                server.update(rates(first['server'], last['server'], ['snapshot_bytes']))
            clients = {p: rates(first["clients"][p], last["clients"][p],
                               ["prediction_ticks", "commands_sent", "snapshots_received", "presentations"])
                       for p in first["clients"]}
            for p in clients:
                clients[p].update(collision_path(first['clients'][p], last['clients'][p]))
                if 'prediction_corrections' in first['clients'][p]:
                    clients[p].update(rates(first['clients'][p], last['clients'][p], ['prediction_steps', 'prediction_corrections']))
                if 'gpu_presents' in first['clients'][p]:
                    clients[p].update(rates(first['clients'][p], last['clients'][p], ['gpu_presents', 'gpu_present_failures']))
            expected = 1000 * len(args.client_ports)
            collision_pass = all(client['collision_pass'] for client in clients.values())
            rate_pass = abs(server["ticks_hz"] - 1000) <= 1 and all(
                abs(server[name] - expected) <= expected * .001
                for name in ["commands_received_hz", "snapshots_sent_hz"])
            rate_pass &= all(abs(c[name] - 1000) <= 1 for c in clients.values()
                             for name in ["prediction_ticks_hz", "commands_sent_hz", "snapshots_received_hz"])
            peer_rates = {}
            if 'peers' in first['server']:
                initial_peers = {p['port']: p for p in first['server']['peers']}
                final_peers = {p['port']: p for p in last['server']['peers']}
                duration_server = last['server']['seconds'] - first['server']['seconds']
                for port in args.client_ports:
                    if port not in initial_peers or port not in final_peers:
                        rate_pass = False
                        continue
                    peer_rates[str(port)] = {name + '_hz': (final_peers[port][name] - initial_peers[port][name]) / duration_server
                                             for name in ('commands', 'snapshots')}
                rate_pass &= len(peer_rates) == len(args.client_ports) and all(abs(hz - 1000) <= 1 for peer in peer_rates.values() for hz in peer.values())
            duration = last["elapsed"] - first["elapsed"]
            cpu = {pid: {"average_cpu_cores": (last["processes"][pid]["cpu_seconds"] -
                                             first["processes"][pid]["cpu_seconds"]) / duration,
                         "peak_private_bytes": max(s["processes"][pid]["private_bytes"] for s in samples),
                         "private_growth_bytes": last["processes"][pid]["private_bytes"] - first["processes"][pid]["private_bytes"]}
                   for pid in first["processes"]}
            playing_fractions = {}
            if 'peers' in first['server']:
                for port, final_peer in final_peers.items():
                    if port in initial_peers and 'playing_ticks' in final_peer:
                        ticks = last['server']['ticks'] - first['server']['ticks']
                        playing_fractions[str(port)] = (final_peer['playing_ticks'] - initial_peers[port]['playing_ticks']) / max(1, ticks)
            server_debt_pass = last['server']['debt'] <= max(8, first['server']['debt'] + 2)
            client_debt = {
                p: {'start_debt': first['clients'][p]['debt'],
                    'end_debt': last['clients'][p]['debt'],
                    'max_sampled_debt': max(s['clients'][p]['debt'] for s in samples),
                    'debt_pass': last['clients'][p]['debt'] <= max(8, first['clients'][p]['debt'] + 2)}
                for p in first['clients']}
            client_debt_pass = all(value['debt_pass'] for value in client_debt.values())
            debt_pass = server_debt_pass and client_debt_pass
            gameplay_pass = (len(playing_fractions) == len(args.client_ports)
                             and all(playing_fractions.get(str(p), 0) >= .2
                                     and clients[str(p)].get('prediction_steps_hz', 0) > 0
                                     for p in args.client_ports))
            owner_evidence = {}
            for port in first['clients']:
                owner_evidence[port] = independent_owner_path([sample['clients'][port] for sample in samples])
            owner_pass = all(owner['passed'] for owner in owner_evidence.values())
            expected_bots = min(args.desired_bots, 16 - len(args.client_ports))
            bot_samples = [sample['server'].get('native_bots', {}) for sample in samples]
            bot_pass = all(bots.get('active') == expected_bots
                           and bots.get('humans') == len(args.client_ports)
                           and bots.get('requested') == args.desired_bots
                           and bots.get('script_bootstrap') for bots in bot_samples)
            bot_rates = {}
            bot_duration = last['server']['seconds'] - first['server']['seconds']
            for name in ('command_ticks', 'playing_ticks', 'aim_ticks', 'respawn_pulses'):
                bot_rates[name + '_hz'] = (bot_samples[-1].get(name, 0) - bot_samples[0].get(name, 0)) / bot_duration
            bot_slot_rates = {}
            initial_bot_slots = {slot['slot']: slot for slot in bot_samples[0].get('slots', [])}
            final_bot_slots = {slot['slot']: slot for slot in bot_samples[-1].get('slots', [])}
            stable_bot_slots = len(initial_bot_slots) == expected_bots and initial_bot_slots.keys() == final_bot_slots.keys()
            for index, slot in initial_bot_slots.items():
                final_slot = final_bot_slots.get(index, {})
                generation = slot.get('generation')
                stable = bool(generation and final_slot.get('generation') == generation)
                hz = (final_slot.get('command_ticks', 0) - slot.get('command_ticks', 0)) / bot_duration
                bot_slot_rates[str(index)] = {'generation': generation, 'command_ticks_hz': hz, 'stable': stable,
                                              'passed': stable and abs(hz - 1000) <= 1}
            bot_controls_pass = (stable_bot_slots and all(slot['passed'] for slot in bot_slot_rates.values())
                                 and abs(bot_rates['command_ticks_hz'] - 1000 * expected_bots) <= max(1, expected_bots))
            bot_pass &= bot_controls_pass
            # The actual launched server has an isolated filesystem home.
            server_log = root / 'runtime' / 'benchmarks' / f'server-{args.server_port}' / 'main' / 'khz_server.log'
            log_text = server_log.read_text(encoding='latin1') if server_log.exists() else ''
            script_errors = log_text.count('******* script runtime error *******')
            rendered = clients[str(args.client_ports[0])]
            initial_render = first['clients'][str(args.client_ports[0])]
            final_render = last['clients'][str(args.client_ports[0])]
            frame_timings = {}
            for name, final_timing in final_render.get('timings', {}).items():
                initial_timing = initial_render['timings'][name]
                count = final_timing['count'] - initial_timing['count']
                total_us = final_timing['total_us'] - initial_timing['total_us']
                frame_timings[name] = {'count': count, 'average_us': total_us / count if count else None,
                                      'wall_seconds_per_second': total_us / 1e6 / duration,
                                      'session_max_us': final_timing['max_us']}
            threads = {tid: {'process_id': thread['process_id'], 'role': thread['role'],
                             'average_cpu_cores': (thread['cpu_seconds'] - first['threads'][tid]['cpu_seconds']) / duration}
                       for tid, thread in last.get('threads', {}).items() if tid in first.get('threads', {})}
            gpu_summary = {}
            for pid in cpu:
                gpu_samples = [(s['elapsed'], s.get('gpu', {}).get('processes', {}).get(pid, {}).get('busiest_engine_percent'))
                               for s in samples[1:]]
                weighted, seconds, peak, previous_elapsed = 0.0, 0.0, None, samples[0]['elapsed']
                for elapsed, value in gpu_samples:
                    interval = elapsed - previous_elapsed
                    previous_elapsed = elapsed
                    if value is not None:
                        weighted += value * interval
                        seconds += interval
                        peak = value if peak is None else max(peak, value)
                gpu_summary[pid] = {'average_busiest_engine_percent': weighted / seconds if seconds else None,
                                    'peak_busiest_engine_percent': peak, 'sampled_seconds': seconds}
            render_present_fps = rendered.get('gpu_presents_hz')
            render_cap_pass = all(s['clients'][str(args.client_ports[0])].get('render_cap') == args.render_fps
                                  for s in samples)
            render_pass = (render_present_fps is not None and render_present_fps > 0
                           and rendered.get('gpu_present_failures_hz', 1) == 0 and render_cap_pass)
            report["summary"] = {"measured_seconds": duration, "server": server, "clients": clients,
                                 "server_peers": peer_rates,
                                 "playing_fractions": playing_fractions,
                                 "gameplay_pass": gameplay_pass,
                                 "independent_owner_pass": owner_pass, "independent_owners": owner_evidence,
                                 "bots_pass": bot_pass, "expected_bots": expected_bots, "bot_rates": bot_rates,
                                 "bot_controls_pass": bot_controls_pass, "bot_slots": bot_slot_rates,
                                 "bot_control_work": bot_samples[-1].get('control_work', {}),
                                 "server_log": str(server_log),
                                 "collision_pass": collision_pass,
                                 "server_script_errors": script_errors,
                                 "script_pass": bool(log_text) and script_errors == 0,
                                 "render_present_fps": render_present_fps,
                                 "render_pass": render_pass,
                                 "render_cap_pass": render_cap_pass,
                                 "rendered_process_id": final_render.get('process_id'),
                                 "render_frame_timings": frame_timings,
                                 "threads": threads, "gpu": gpu_summary,
                                 "gpu_counter_errors": sorted({s['gpu']['error'] for s in samples if not s['gpu']['available']}),
                                 "processes": cpu, "rates_pass": bool(rate_pass),
                                 "p99_pass": last["server"]["work_p99_us"] < 1000,
                                 "debt_pass": debt_pass,
                                 "server_debt_pass": server_debt_pass,
                                 "client_debt_pass": client_debt_pass,
                                 "client_debt": client_debt,
                                 "start_debt": first["server"]["debt"], "end_debt": last["server"]["debt"],
                                 "max_sampled_debt": max(s["server"]["debt"] for s in samples),
                                 "complete": duration >= args.seconds - 1 and not report["errors"]}
        args.output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
        print(json.dumps(report.get("summary", report["errors"]), indent=2), flush=True)
    return 0 if report.get("summary", {}).get("complete") else 1


if __name__ == "__main__":
    raise SystemExit(main())
