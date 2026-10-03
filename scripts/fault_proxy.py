"""Seeded, bounded localhost UDP impairment for the real custom protocol pair."""
import argparse
import heapq
import json
from pathlib import Path
import random
import select
import socket
import time


def run(args):
    rng = random.Random(args.seed)
    target = ('127.0.0.1', args.target_port)
    client = ('127.0.0.1', args.client_port)
    queue = []
    queued_bytes = 0
    serial = 0
    last_delivered = {'up': 0, 'down': 0}
    stats = {'received': 0, 'sent': 0, 'lost': 0, 'duplicates': 0,
             'reordered': 0, 'overflow': 0, 'peak_packets': 0, 'peak_bytes': 0,
             'seed': args.seed, 'packet_capacity': 8192, 'byte_capacity': 8 * 1024 * 1024}
    start = time.monotonic()
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4 * 1024 * 1024)
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 4 * 1024 * 1024)
        sock.bind(('127.0.0.1', args.port))
        sock.setblocking(False)
        print(f'Proxy 127.0.0.1:{args.port} -> {target[1]}, client {client[1]}', flush=True)
        while time.monotonic() - start < args.seconds:
            now = time.monotonic()
            while queue and queue[0][0] <= now:
                _, number, direction, destination, data = heapq.heappop(queue)
                queued_bytes -= len(data)
                try:
                    sock.sendto(data, destination)
                    stats['sent'] += 1
                    if number < last_delivered[direction]:
                        stats['reordered'] += 1
                    last_delivered[direction] = max(number, last_delivered[direction])
                except (BlockingIOError, ConnectionResetError):
                    stats['overflow'] += 1
            timeout = min(.002, max(0, queue[0][0] - now)) if queue else .002
            if not select.select([sock], [], [], timeout)[0]:
                continue
            # Bound receive work so floods cannot starve deadlines or cleanup.
            for _ in range(256):
                try:
                    data, sender = sock.recvfrom(65536)
                except (BlockingIOError, ConnectionResetError):
                    break
                if sender == target:
                    direction, destination = 'down', client
                elif sender == client:
                    direction, destination = 'up', target
                else:
                    continue
                stats['received'] += 1
                elapsed = time.monotonic() - start
                # Keep challenge, stats and connectionless handshakes intact.
                impaired = args.warmup <= elapsed < args.warmup + args.impair_seconds and not data.startswith(b'\xff' * 4)
                if impaired and rng.random() < args.loss:
                    stats['lost'] += 1
                    continue
                copies = 2 if impaired and rng.random() < args.duplicate else 1
                stats['duplicates'] += copies - 1
                for _ in range(copies):
                    if len(queue) >= 8192 or queued_bytes + len(data) > stats['byte_capacity']:
                        stats['overflow'] += 1
                        continue
                    delay = rng.uniform(0, args.jitter_ms / 1000) if impaired else 0
                    serial += 1
                    heapq.heappush(queue, (time.monotonic() + delay, serial, direction, destination, data))
                    queued_bytes += len(data)
                stats['peak_packets'] = max(stats['peak_packets'], len(queue))
                stats['peak_bytes'] = max(stats['peak_bytes'], queued_bytes)
    stats.update(seconds=time.monotonic() - start, queued_at_exit=len(queue),
                 warmup=args.warmup, impair_seconds=args.impair_seconds,
                 loss=args.loss, duplicate=args.duplicate, jitter_ms=args.jitter_ms)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(stats, indent=2), encoding='utf8')
    print(json.dumps(stats), flush=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--port', type=int, default=28970)
    parser.add_argument('--target-port', type=int, default=28971)
    parser.add_argument('--client-port', type=int, default=28972)
    parser.add_argument('--seconds', type=float, default=30)
    parser.add_argument('--warmup', type=float, default=8)
    parser.add_argument('--impair-seconds', type=float, default=8)
    parser.add_argument('--loss', type=float, default=.01)
    parser.add_argument('--duplicate', type=float, default=.02)
    parser.add_argument('--jitter-ms', type=float, default=4)
    parser.add_argument('--seed', type=int, default=1000)
    parser.add_argument('--output', type=Path, required=True)
    options = parser.parse_args()
    if not (0 <= options.loss <= 1 and 0 <= options.duplicate <= 1 and 0 <= options.jitter_ms <= 250):
        parser.error('Invalid impairment probabilities or jitter')
    if len({options.port, options.target_port, options.client_port}) != 3 or any(not 1024 <= p <= 65535 for p in (options.port, options.target_port, options.client_port)):
        parser.error('Use three distinct unprivileged ports')
    if options.seconds <= options.warmup + options.impair_seconds or options.warmup < 0 or options.impair_seconds < 0:
        parser.error('Include a positive recovery period')
    run(options)
