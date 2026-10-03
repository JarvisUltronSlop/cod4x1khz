"""Create owned ZIP/loose/junction fixtures and run the private FS test binary.

This script never builds, deploys or launches the game. Its result directory
retains native output, exact fixture metadata and tested source/binary hashes.
"""
import argparse
import base64
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import time
import zipfile
import zlib

PROJECT = Path(__file__).resolve().parent.parent


def crc_byte(crc, value):
    crc ^= value
    for _ in range(8):
        crc = (crc >> 1) ^ (0xedb88320 if crc & 1 else 0)
    return crc & 0xffffffff


def encrypt(payload, password, crc):
    keys = [0x12345678, 0x23456789, 0x34567890]
    def advance(value):
        keys[0] = crc_byte(keys[0], value)
        keys[1] = ((keys[1] + (keys[0] & 255)) * 134775813 + 1) & 0xffffffff
        keys[2] = crc_byte(keys[2], keys[1] >> 24)
    for value in password:
        advance(value)
    output = bytearray()
    for value in bytes(range(11)) + bytes([crc >> 24]) + payload:
        temp = (keys[2] & 0xffff) | 2
        output.append(value ^ (((temp * (temp ^ 1)) >> 8) & 255))
        advance(value)
    return bytes(output)


def archive(path):
    entries = [('order.dat', b'archive-wins', 0, None, False),
               ('case.dat', b'first-case-entry', 0, None, False),
               ('CASE.DAT', b'last-case-entry', 0, None, False),
               ('deflated.dat', b'compressed-worker-data', 8, None, False),
               ('empty.dat', b'', 0, None, False),
               ('corrupt.dat', b'bad-crc-data', 0, None, True),
               ('encrypted.dat', b'encrypted-worker-bytes', 8, b'classic-secret', False)]
    local = bytearray()
    central = bytearray()
    metadata = []
    for name, plain, method, password, corrupt in entries:
        filename = name.encode('ascii')
        crc = zlib.crc32(plain)
        if corrupt:
            crc ^= 1
        if method == 8:
            compressor = zlib.compressobj(wbits=-15)
            payload = compressor.compress(plain) + compressor.flush()
        else:
            payload = plain
        flags = 1 if password is not None else 0
        if password is not None:
            payload = encrypt(payload, password, crc)
        offset = len(local)
        local += struct.pack('<IHHHHHIIIHH', 0x04034b50, 20, flags, method, 0, 0,
                             crc, len(payload), len(plain), len(filename), 0) + filename + payload
        central += struct.pack('<IHHHHHHIIIHHHHHII', 0x02014b50, 20, 20, flags, method, 0, 0,
                               crc, len(payload), len(plain), len(filename), 0, 0, 0, 0, 0, offset) + filename
        metadata.append({'name': name, 'bytes': len(plain), 'method': method,
                         'classic_encrypted': password is not None, 'bad_crc': corrupt})
    end = struct.pack('<IHHHHIIH', 0x06054b50, 0, 0, len(entries), len(entries), len(central), len(local), 0)
    path.write_bytes(local + central + end)
    # Independent standard-library decoder verifies the fixture generator's
    # actual encrypted/deflated bytes before the native implementation runs.
    with zipfile.ZipFile(path) as zipped:
        assert zipped.read('encrypted.dat', pwd=b'classic-secret') == b'encrypted-worker-bytes'
        assert zipped.read('deflated.dat') == b'compressed-worker-data'
        try:
            zipped.read('corrupt.dat')
        except zipfile.BadZipFile:
            pass
        else:
            raise AssertionError('corrupt fixture must fail independent CRC validation')
    return metadata


def fixture(root):
    home, base = root / 'home', root / 'base'
    (home / 'main/dir').mkdir(parents=True)
    (base / 'main').mkdir(parents=True)
    (root / 'outside').mkdir()
    files = {'home/main/order.dat': b'home-loose', 'home/main/home.txt': b'home-game',
             'base/main/base.txt': b'base-game', 'home/main/dir/nested.txt': b'normalized-slash',
             'home/sv.txt': b'alpha\nbeta', 'base/sv-base.txt': b'sv-base',
             'home/main/allowed.cfg': b'cfg-exception', 'outside/outside.txt': b'outside-home'}
    for name, data in files.items():
        (root / name).write_bytes(data)
    for name, size in [('maximum.bin', 16 * 1024 * 1024), ('oversized.bin', 16 * 1024 * 1024 + 1)]:
        with (home / 'main' / name).open('wb') as stream:
            stream.seek(size - 1)
            stream.write(b'\0')
    entries = archive(home / 'main/ordered.iwd')
    junction = home / 'main/escape'
    link_method = None
    try:
        os.symlink(root / 'outside', junction, target_is_directory=True)
        link_method = 'native directory symlink'
    except OSError:
        # Native PowerShell creates only this newly owned junction. Keep it as
        # evidence; do not recursively delete a path containing a reparse link.
        quote = lambda value: "'" + str(value).replace("'", "''") + "'"
        command = 'New-Item -ItemType Junction -Path ' + quote(junction) + ' -Target ' + quote(root / 'outside') + ' -ErrorAction Stop | Out-Null'
        encoded = base64.b64encode(command.encode('utf-16le')).decode('ascii')
        native = subprocess.run(['powershell', '-NoProfile', '-EncodedCommand', encoded], capture_output=True, timeout=5)
        if native.returncode == 0:
            link_method = 'native directory junction'
    return {'archive_entries': entries, 'reparse_fixture': link_method,
            'fixture_files_sha256': {name: hashlib.sha256(data).hexdigest() for name, data in files.items()}}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('binary', type=Path)
    arguments = parser.parse_args()
    executable = arguments.binary.resolve(strict=True)
    output = PROJECT / 'results' / ('plugin-fs-' + time.strftime('%Y%m%d-%H%M%S') + f'-{time.time_ns() % 1000000:06d}')
    root = output / 'fixture'
    root.mkdir(parents=True)
    result = {'scope': 'Actual isolated private plugin filesystem backend, not native DLL integration',
              'binary': str(executable), 'sha256': {str(executable): hashlib.sha256(executable.read_bytes()).hexdigest()}}
    started = time.monotonic()
    try:
        result['fixture'] = fixture(root)
        native = subprocess.run([str(executable), str(root)], cwd=PROJECT, capture_output=True, timeout=20)
        (output / 'native-tests.log').write_bytes(native.stdout + native.stderr)
        result['exit_code'] = native.returncode
        result['passed'] = native.returncode == 0 and b'failures=0' in native.stdout
    except Exception as error:
        result['error'] = str(error)
        result['passed'] = False
    for relative in ('engine/src/khz/khz_plugin_fs.h', 'engine/src/khz/khz_plugin_fs.cpp',
                     'engine/src/khz/khz_plugin_fs_adapter.cpp', 'engine/src/khz/khz_plugin_unzip.c',
                     'engine/src/khz/khz_plugin_zip.h', 'engine/src/khz/khz_plugin_zip_prefix.h',
                     'engine/src/khz/khz_plugin_files.inl', 'sdk/include/public_file_api.h',
                     'upstream/cod4x_server/src/zlib/unzip.c', 'upstream/cod4x_server/src/zlib/crypt.h',
                     'engine/tests/khz_plugin_fs_tests.cpp', 'scripts/plugin_fs_probe.py'):
        path = PROJECT / relative
        result['sha256'][str(path)] = hashlib.sha256(path.read_bytes()).hexdigest()
    result['elapsed_seconds'] = time.monotonic() - started
    (output / 'result.json').write_text(json.dumps(result, indent=2))
    print(json.dumps({'output': str(output), 'passed': result['passed'], 'elapsed_seconds': result['elapsed_seconds'],
                      'error': result.get('error')}, indent=2))
    return 0 if result['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
