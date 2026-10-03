"""Restore a private snapshot into a new directory; verify every restored file."""
import argparse
from collections import defaultdict
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import shutil
import subprocess
import zipfile

BLOCK = 1024 * 1024
REPO = 'JarvisUltronSlop/cod4x1khz'


def sha256(path):
    digest = hashlib.sha256()
    with path.open('rb') as stream:
        while chunk := stream.read(BLOCK):
            digest.update(chunk)
    return digest.hexdigest()


def safe_path(root, name):
    relative = PurePosixPath(name)
    if relative.is_absolute() or '..' in relative.parts or '\\' in name or ':' in name:
        raise RuntimeError(f'Unsafe snapshot path: {name}')
    resolved = (root / Path(*relative.parts)).resolve()
    if not resolved.is_relative_to(root):
        raise RuntimeError(f'Snapshot path escapes destination: {name}')
    return resolved


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--manifest', type=Path, required=True)
    parser.add_argument('--destination', type=Path, required=True)
    parser.add_argument('--archives', type=Path)
    args = parser.parse_args()
    manifest = json.loads(args.manifest.read_text(encoding='utf-8-sig'))
    if manifest.get('schema') != 1 or manifest.get('repository') != REPO or not manifest.get('private'):
        raise RuntimeError('Unexpected snapshot format or repository')
    destination = args.destination.resolve()
    if destination.exists() and any(destination.iterdir()):
        raise RuntimeError('Restore destination must be new or empty')
    # Validate all names before creating or downloading anything.
    groups = defaultdict(list)
    for name, record in manifest['files'].items():
        safe_path(destination, name)
        if Path(record['archive_name']).name != record['archive_name']:
            raise RuntimeError('Unsafe archive name')
        groups[(record['archive_tag'], record['archive_name'])].append((name, record))
    for name in manifest['directories']:
        safe_path(destination, name)
    destination.mkdir(parents=True, exist_ok=True)
    staging = destination / '.restore-staging'
    if '.restore-staging' in manifest['directories']:
        raise RuntimeError('Reserved staging path in snapshot')
    staging.mkdir()
    gh = shutil.which('gh')
    if not gh:
        local = Path(__file__).resolve().parents[1] / '.deps/github-cli/bin/gh.exe'
        gh = str(local) if local.exists() else None
    if not args.archives and not gh:
        raise RuntimeError('Install/authenticate GitHub CLI or provide --archives')
    verified = 0
    archive_records = {v['name']: v for v in manifest['volumes']}
    for index, ((tag, archive_name), entries) in enumerate(sorted(groups.items()), 1):
        archive = args.archives.resolve() / archive_name if args.archives else staging / archive_name
        print(f'Volume {index}/{len(groups)}: {archive_name}', flush=True)
        if args.archives:
            if not archive.is_file():
                raise RuntimeError(f'Missing archive: {archive}')
            expected = archive_records.get(archive_name, {}).get('sha256')
        else:
            release = json.loads(subprocess.check_output([gh, 'api', f'repos/{REPO}/releases/tags/{tag}']))
            asset = next(a for a in release['assets'] if a['name'] == archive_name)
            expected = asset.get('digest', '').removeprefix('sha256:')
            if len(expected) != 64:
                raise RuntimeError('Remote archive digest is unavailable')
            subprocess.run([gh, 'release', 'download', tag, '--repo', REPO, '--pattern', archive_name,
                            '--dir', str(staging)], check=True)
        if expected and sha256(archive) != expected:
            raise RuntimeError(f'Archive hash mismatch: {archive_name}')
        with zipfile.ZipFile(archive) as source:
            for name, record in entries:
                target = safe_path(destination, name)
                target.parent.mkdir(parents=True, exist_ok=True)
                digest, total = hashlib.sha256(), 0
                with source.open(name) as incoming, target.open('xb') as outgoing:
                    while chunk := incoming.read(BLOCK):
                        digest.update(chunk)
                        total += len(chunk)
                        outgoing.write(chunk)
                if total != record['size'] or digest.hexdigest() != record['sha256']:
                    raise RuntimeError(f'Restored file hash mismatch: {name}')
                os.utime(target, ns=(record['mtime_ns'], record['mtime_ns']))
                verified += 1
        if not args.archives:
            archive.unlink()
    for name in manifest['directories']:
        safe_path(destination, name).mkdir(parents=True, exist_ok=True)
    staging.rmdir()
    if verified != manifest['total_files']:
        raise RuntimeError('Restored file count does not match the manifest')
    print(f'Restored and SHA-256 verified {verified} files in {destination}', flush=True)


if __name__ == '__main__':
    main()
