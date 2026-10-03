"""Loopback UDP checks for fragmented engine diagnostic replies; no game needed."""
from contextlib import contextmanager
import json
from pathlib import Path
import socket
import sys
import threading
import time
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
from monitor import query


PRINT_PREFIX = b'\xff' * 4 + b'print\n'


@contextmanager
def sender(handler):
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as server:
        server.bind(('127.0.0.1', 0))
        server.settimeout(1)
        errors = []

        def reply():
            try:
                _, destination = server.recvfrom(65536)
                handler(server, destination)
            except Exception as error:
                errors.append(error)

        thread = threading.Thread(target=reply, daemon=True)
        thread.start()
        try:
            yield server.getsockname()[1]
        finally:
            thread.join(timeout=1)
            if thread.is_alive():
                raise RuntimeError('Loopback sender did not finish')
            if errors:
                raise errors[0]


class TelemetryQueryTests(unittest.TestCase):
    def test_complete_client_reply(self):
        with sender(lambda sock, address: sock.sendto(
                b'\xff' * 4 + b'KHZ_JSON {"ok":true}', address)) as port:
            self.assertEqual(query(port, 'khz_stats'), {'ok': True})

    def test_split_marker_and_large_json_body(self):
        expected = {'ok': True, 'body': 'x' * 4000}
        body = ('KHZ_JSON ' + json.dumps(expected)).encode('ascii')
        cuts = (0, 4, 7, 1294, 2588, len(body))

        def reply(sock, address):
            for start, end in zip(cuts, cuts[1:]):
                sock.sendto(PRINT_PREFIX + body[start:end], address)

        with sender(reply) as port:
            self.assertEqual(query(port, 'rcon test khz_json'), expected)

    def test_filters_wrong_port_and_wrong_loopback_address(self):
        def reply(sock, address):
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as foreign_port:
                foreign_port.bind(('127.0.0.1', 0))
                foreign_port.sendto(PRINT_PREFIX + b'KHZ_JSON {"wrong_port":true}', address)
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as foreign_ip:
                foreign_ip.bind(('127.0.0.2', sock.getsockname()[1]))
                foreign_ip.sendto(PRINT_PREFIX + b'KHZ_JSON {"wrong_ip":true}', address)
            sock.sendto(PRINT_PREFIX + b'KHZ_JSON {"ok":true}', address)

        with sender(reply) as port:
            self.assertEqual(query(port, 'rcon test khz_json'), {'ok': True})

    def test_truncated_reply_times_out(self):
        with sender(lambda sock, address: sock.sendto(
                PRINT_PREFIX + b'KHZ_JSON {"missing_end":true', address)) as port:
            with self.assertRaises(TimeoutError):
                query(port, 'rcon test khz_json', timeout=.1)

    def test_invalid_reply_times_out(self):
        with sender(lambda sock, address: sock.sendto(
                PRINT_PREFIX + b'KHZ_JSON {"invalid":not_json}', address)) as port:
            with self.assertRaises(TimeoutError):
                query(port, 'rcon test khz_json', timeout=.1)

    def test_partial_reply_does_not_extend_absolute_deadline(self):
        def reply(sock, address):
            time.sleep(.2)
            sock.sendto(PRINT_PREFIX + b'KHZ_JSON {"still_partial":', address)

        with sender(reply) as port:
            started = time.monotonic()
            with self.assertRaises(TimeoutError):
                query(port, 'rcon test khz_json', timeout=.3)
            elapsed = time.monotonic() - started
        self.assertLess(elapsed, .45)

    def test_accumulation_is_bounded(self):
        def reply(sock, address):
            sock.sendto(PRINT_PREFIX + b'KHZ_JSON {"unfinished":"', address)
            for _ in range(7):
                sock.sendto(PRINT_PREFIX + b'x' * 10000, address)

        with sender(reply) as port:
            with self.assertRaisesRegex(ValueError, 'exceeds 65536 bytes'):
                query(port, 'rcon test khz_json')


if __name__ == '__main__':
    unittest.main()
