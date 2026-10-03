"""Exercise native unlock/restore transactions in an isolated benchmark profile."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import struct
import time
import uuid
from monitor import query


def fnv(data):
    value = 2166136261
    for byte in data:
        value = ((value ^ byte) * 16777619) & 0xffffffff
    return value


def read_stat(data, index):
    return data[4 + index] if index < 2000 else struct.unpack_from('<I', data, 2004 + 4 * (index - 2000))[0]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', type=int, default=28994)
    parser.add_argument('--profile-dir', type=Path, required=True)
    parser.add_argument('--locked-backup', required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    project = Path(__file__).resolve().parent.parent
    profile = args.profile_dir.resolve(strict=True)
    profile.relative_to((project / 'runtime' / 'benchmarks').resolve(strict=True))
    backups = profile / 'khz_backups'
    locked = backups / args.locked_backup
    if locked.parent != backups or len((locked / 'memory_stats.bin').read_bytes()) != 8192:
        raise ValueError('An existing complete backup in the isolated profile is required')
    result = {'scope': 'Actual native profile transactions; class/loadout and connected refresh tests are separate',
              'binary_sha256': hashlib.sha256((project / 'cod4x1khz.exe').read_bytes()).hexdigest(),
              'profile': str(profile), 'cases': []}
    owned_copies = []
    restore_name = None

    def check(name, passed, evidence):
        result['cases'].append({'name': name, 'passed': bool(passed), 'evidence': evidence})

    def status():
        return query(args.port, 'khz_ui status', 'KHZ_UI', 3)['profile']

    def console(text):
        queued = query(args.port, 'khz_ui console ' + text, 'KHZ_UI', 3)
        if queued['result'] != 'queued':
            raise RuntimeError('Native command was not queued: ' + text)
        time.sleep(.15)
        return status()

    def unlock_backup():
        before = set(backups.iterdir())
        queued = query(args.port, 'khz_ui unlockall', 'KHZ_UI', 3)
        if queued['result'] != 'queued':
            raise RuntimeError('Native unlock command was not queued')
        until = time.monotonic() + 5
        while time.monotonic() < until:
            created = set(backups.iterdir()) - before
            if len(created) == 1 and (next(iter(created)) / 'backup.txt').is_file():
                return next(iter(created))
            time.sleep(.05)
        raise RuntimeError('Native unlock did not complete a backup')

    try:
        before = status()
        if query(args.port, 'khz_ui status', 'KHZ_UI')['host']['owned']:
            raise RuntimeError('Return to the main menu before profile tests')
        safety = unlock_backup()
        restore_name = safety.name
        saved_memory = (safety / 'memory_stats.bin').read_bytes()
        check('Native safety backup checksum and identity',
              f'memory_stats_fnv1a={fnv(saved_memory)}\n' in (safety / 'backup.txt').read_text() and len(saved_memory) == 8192,
              {'backup': str(safety), 'sha256': hashlib.sha256(saved_memory).hexdigest()})
        check('Idempotent unlock preserves combat/challenges', status()['preserved_stats_hash'] == before['preserved_stats_hash'], status())
        locked_status = console('restoreunlocks ' + locked.name)
        locked_memory = (locked / 'memory_stats.bin').read_bytes()
        check('Native locked profile restore', not locked_status['all_unlocks'] and locked_status['xp'] == read_stat(locked_memory, 2301), locked_status)
        pristine = unlock_backup()
        new_status = status()
        unlocked_snapshot = unlock_backup()
        candidate = (unlocked_snapshot / 'memory_stats.bin').read_bytes()
        record = (profile / 'khz_unlocks.bin').read_bytes()
        magic, version, count, minimum, checksum = struct.unpack_from('<5I', record)
        masks = struct.unpack_from('<3498I', record, 20)
        zero_checksum = record[:16] + b'\0' * 4 + record[20:]
        permitted = {2301} | {index for index, mask in enumerate(masks) if mask}
        changes = [i for i in range(3498) if read_stat(locked_memory, i) != read_stat(candidate, i)]
        untouched = [i for i in range(3498) if i not in permitted and read_stat(locked_memory, i) != read_stat(candidate, i)]
        ownership = all((read_stat(candidate, i) & mask) == mask for i, mask in enumerate(masks))
        check('Native unlock changes ownership and XP only', not untouched and ownership and read_stat(candidate, 2301) >= minimum,
              {'changed_indices': changes, 'unexpected_indices': untouched, 'before': locked_status, 'after': new_status})
        check('Persisted separate entitlement matches native stats', (magic, version, count) == (0x3155484b, 1, 3498) and fnv(zero_checksum) == checksum,
              {'bytes': len(record), 'sha256': hashlib.sha256(record).hexdigest(), 'minimum_xp': minimum})
        repeated_snapshot = unlock_backup()
        repeated_memory = (repeated_snapshot / 'memory_stats.bin').read_bytes()
        check('Idempotent unlock preserves every stat', candidate[4:] == repeated_memory[4:],
              {'first_payload_sha256': hashlib.sha256(candidate[4:]).hexdigest(),
               'repeated_payload_sha256': hashlib.sha256(repeated_memory[4:]).hexdigest()})
        for field in ('memory', 'entitlement'):
            copy = backups / ('khz-fixture-' + uuid.uuid4().hex)
            copy.mkdir()
            owned_copies.append(copy)
            for name in ('memory_stats.bin', 'backup.txt', 'khz_unlocks.bin'):
                source = unlocked_snapshot / name
                if source.is_file():
                    shutil.copyfile(source, copy / name)
            target = copy / ('memory_stats.bin' if field == 'memory' else 'khz_unlocks.bin')
            corrupt = bytearray(target.read_bytes())
            corrupt[100 if field == 'memory' else 20] ^= 1
            target.write_bytes(corrupt)
            before_files = {name: hashlib.sha256((profile / name).read_bytes()).hexdigest() for name in ('mpdata', 'khz_unlocks.bin')}
            before_status = status()
            after_status = console('restoreunlocks ' + copy.name)
            after_files = {name: hashlib.sha256((profile / name).read_bytes()).hexdigest() for name in before_files}
            check('Corrupt ' + field + ' restore rejected without profile writes', before_files == after_files and before_status == after_status,
                  {'before_files': before_files, 'after_files': after_files, 'profile': after_status})
        restored = console('restoreunlocks ' + safety.name)
        # An idempotent unlock backs up the actual restored live buffer for an
        # exact comparison. Native checksum headers can be refreshed when a
        # dirty live buffer is encoded, so compare the entire stat payload.
        roundtrip = unlock_backup()
        check('Native restore roundtrip preserves every live stat', (roundtrip / 'memory_stats.bin').read_bytes()[4:] == saved_memory[4:],
              {'before': before, 'after': restored, 'restored_backup': str(roundtrip)})
    except Exception as error:
        result['error'] = str(error)
    finally:
        if restore_name:
            try:
                result['final_profile'] = console('restoreunlocks ' + restore_name)
            except Exception as error:
                result['restore_error'] = str(error)
        for path in owned_copies:
            # Only the exact files created by this probe are removed.
            for name in ('memory_stats.bin', 'backup.txt', 'khz_unlocks.bin'):
                (path / name).unlink(missing_ok=True)
            path.rmdir()
    result['passed'] = bool(result['cases']) and not result.get('error') and not result.get('restore_error') and all(c['passed'] for c in result['cases'])
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2), encoding='utf8')
    print(json.dumps({'passed': result['passed'], 'cases': len(result['cases']), 'error': result.get('error'), 'output': str(args.output)}, indent=2))
    return 0 if result['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
