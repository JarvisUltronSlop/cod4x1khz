"""Exclude connection/loading work from the server's steady-match histogram."""
import socket
import time
from monitor import query

with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
    sock.sendto(b'\xff' * 4 + b'rcon local-khz-benchmark khz_resetstats', ('127.0.0.1', 28960))
time.sleep(1.1)  # server RCON rate limit
stats = query(28960, 'rcon local-khz-benchmark khz_json')
if stats['seconds'] > 3:
    raise SystemExit('Server statistics were not reset')
time.sleep(1.1)
