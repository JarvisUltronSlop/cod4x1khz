import argparse
import time
from monitor import independent_owner_path, query

parser = argparse.ArgumentParser()
parser.add_argument('--ports', type=int, nargs='+', required=True)
parser.add_argument('--timeout', type=float, default=30)
parser.add_argument('--server-port', type=int)
parser.add_argument('--desired-bots', type=int, default=15)
args = parser.parse_args()
until = time.monotonic() + args.timeout
pending = set(args.ports)
owner_samples = {port: [] for port in args.ports}
bots_ready = args.server_port is None
next_server_query = 0.0
while (pending or not bots_ready) and time.monotonic() < until:
    for port in list(pending):
        try:
            state = query(port, 'khz_state', 'KHZ_STATE', .3)
            if (state['connection'] == 9 and state['pm_type'] == 0 and state['weapon']
                    and not (state.get('other_flags', 0) & 2)):
                owner_samples[port].append(query(port, 'khz_stats', timeout=.3))
                owner_samples[port] = owner_samples[port][-3:]
                owner = independent_owner_path(owner_samples[port])
                if owner['passed'] and owner['ticks'] >= 500 and owner['pose_ticks'] >= 500:
                    print(f'Client {port} active with independent simulation owner, weapon {state["weapon"]}', flush=True)
                    pending.remove(port)
            else:
                owner_samples[port].clear()
        except (OSError, ValueError):
            pass
    if args.server_port is not None and time.monotonic() >= next_server_query:
        next_server_query = time.monotonic() + 1.1
        try:
            stats = query(args.server_port, 'rcon local-khz-benchmark khz_json', timeout=.5)
            bots = stats.get('native_bots', {})
            expected = min(args.desired_bots, 16 - len(args.ports))
            slots = bots.get('slots', [])
            bots_ready = (bots.get('script_bootstrap') is True and bots.get('requested') == args.desired_bots
                          and bots.get('humans') == len(args.ports) and bots.get('active') == expected
                          and len(slots) == expected and all(slot.get('generation', 0) > 0
                                                           and slot.get('command_ticks', 0) > 0 for slot in slots))
        except (OSError, ValueError):
            bots_ready = False
    if pending or not bots_ready:
        time.sleep(.5)
if pending:
    raise SystemExit(f'Clients did not spawn with a stable independent simulation owner: {sorted(pending)}')
if not bots_ready:
    raise SystemExit('Server did not reach the requested native bot population and advancing controls')
