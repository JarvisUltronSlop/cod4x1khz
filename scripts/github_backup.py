"""Private, checked source and workspace backups. No engine or renderer changes.

Each release manifest is a complete inventory. Unchanged files retain references
to older verified volumes; changed files get new independent ZIP volumes. This
avoids uploading owned retail data every hour. Local staging is removed only
after GitHub confirms the uploaded archive's size and SHA-256.
"""
from __future__ import annotations

import argparse
import ctypes
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time
from datetime import datetime, timezone
import zipfile

ROOT = Path(__file__).resolve().parents[1]
STATE = ROOT / '.backup'
REPO = 'JarvisUltronSlop/cod4x1khz'
LIMIT = 512 * 1024 * 1024
BLOCK = 1024 * 1024
SECRET_NAMES = {'.env', 'hosts.yml', 'credentials', 'id_rsa', 'id_ed25519'}
SECRET_SUFFIXES = {'.pem', '.pfx', '.p12', '.key'}
STORE_SUFFIXES = {'.iwd', '.ff', '.zip', '.nupkg', '.png', '.jpg', '.jpeg', '.bik', '.gz', '.7z'}
SOURCE_DIRS = {'engine', 'upstream', 'native', 'tests', 'config', 'scripts', 'docs', 'sdk', 'staged', 'cmake'}
BUILD_DIRS = {'bin', 'build', 'release', 'debug', '__pycache__', '.git'}
BUILD_SUFFIXES = {'.pyc', '.obj', '.pdb', '.ilk', '.tlog', '.suo', '.user', '.log'}


def emit(message):
    print(message, flush=True)


def run(args, *, capture=True, input=None, env=None, timeout=90):
    result = subprocess.run([str(a) for a in args], cwd=ROOT, input=input,
                            stdout=subprocess.PIPE if capture else None,
                            stderr=subprocess.PIPE if capture else None, env=env, timeout=timeout)
    if result.returncode:
        detail = (result.stderr or b'').decode('utf-8', 'replace').strip()
        raise RuntimeError(f'{args[0]} {args[1]} failed ({result.returncode}): {detail}')
    return (result.stdout or b'').decode('utf-8', 'replace').strip()


def atomic_json(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + '.tmp')
    temporary.write_text(json.dumps(value, indent=2) + '\n', encoding='utf-8')
    os.replace(temporary, path)


def hash_file(path):
    digest = hashlib.sha256()
    with path.open('rb') as stream:
        while chunk := stream.read(BLOCK):
            digest.update(chunk)
    return digest.hexdigest()


def inventory():
    files, directories, excluded = {}, [], []
    for directory, subdirs, names in os.walk(ROOT):
        relative = Path(directory).relative_to(ROOT)
        if relative == Path('.'):
            subdirs[:] = sorted(d for d in subdirs if d not in {'.git', '.backup'})
        else:
            subdirs.sort()
        directories.append(relative.as_posix())
        for name in sorted(names):
            path = Path(directory) / name
            key = path.relative_to(ROOT).as_posix()
            if name in SECRET_NAMES or name.startswith('.env.') or path.suffix.lower() in SECRET_SUFFIXES:
                excluded.append(key)
                continue
            if path.is_symlink():
                raise RuntimeError(f'Cannot silently follow a link in the backup: {key}')
            stat = path.stat()
            files[key] = {'size': stat.st_size, 'mtime_ns': stat.st_mtime_ns,
                          'mode': stat.st_mode}
    return files, directories, excluded


def source_paths(files):
    selected = []
    for name in files:
        path = Path(name)
        if any(part.lower() in BUILD_DIRS for part in path.parts):
            continue
        if path.suffix.lower() in BUILD_SUFFIXES:
            continue
        if path.parts[0] in SOURCE_DIRS or len(path.parts) == 1 and path.suffix.lower() not in {'.exe', '.dll'}:
            selected.append(name)
    return selected


def stage_source(files):
    selected = source_paths(files)
    # git add silently skips paths inside existing nested repositories. Populate
    # the parent index with real blobs, then verify every selected file is there.
    payload = ''.join(json.dumps(name, ensure_ascii=False) + '\n' for name in selected).encode('utf-8')
    hashes = run(['git', 'hash-object', '-w', '--no-filters', '--stdin-paths'], input=payload).splitlines()
    if len(hashes) != len(selected):
        raise RuntimeError('Source blob count does not match the selected files')
    index = b''.join(f'100644 {digest}\t{name}\0'.encode('utf-8') for name, digest in zip(selected, hashes))
    run(['git', 'update-index', '--add', '-z', '--index-info'], input=index)
    tracked = run(['git', 'ls-files', '-z']).split('\0')
    removed = [name for name in tracked if name and not (ROOT / name).exists()]
    if removed:
        run(['git', 'update-index', '--force-remove', '-z', '--stdin'],
            input=b''.join(name.encode('utf-8') + b'\0' for name in removed))
    stage = run(['git', 'ls-files', '--stage', '-z']).split('\0')
    staged = {}
    for entry in stage:
        if entry:
            metadata, name = entry.split('\t', 1)
            staged[name] = metadata.split()
    if any(value[0] == '160000' for value in staged.values()):
        raise RuntimeError('Source must be tracked as files, not embedded repository links')
    for name, digest in zip(selected, hashes):
        if name not in staged or staged[name][1] != digest:
            raise RuntimeError(f'Source was not staged exactly: {name}')
    return bool(run(['git', 'diff', '--cached', '--name-only']))


class GitHub:
    def __init__(self):
        local = ROOT / '.deps/github-cli/bin/gh.exe'
        self.gh = local if local.exists() else shutil.which('gh')
        if not self.gh:
            raise RuntimeError('GitHub CLI is required; authenticate the JarvisUltronSlop account first')
        self.env = dict(os.environ)
        self.env['GH_PROMPT_DISABLED'] = '1'
        self.env['GH_PAGER'] = 'cat'
        # Credential material stays in process memory and is never logged or archived.
        if not self.env.get('GH_TOKEN'):
            self.env['GH_TOKEN'] = run([self.gh, 'auth', 'token', '--hostname', 'github.com'])
        # The REST /user endpoint was returning prolonged 503s during setup;
        # GraphQL verifies the same authenticated identity without that endpoint.
        login = json.loads(self.call('api', 'graphql', '-f', 'query=query { viewer { login } }'))['data']['viewer']['login']
        if login != 'JarvisUltronSlop':
            raise RuntimeError(f'Wrong GitHub account: {login}')
        repo = self.api(f'repos/{REPO}')
        if not repo['private']:
            raise RuntimeError('Full workspace backups require this repository to stay private')

    def call(self, *args):
        return run([self.gh, *args], env=self.env, timeout=600 if args[:2] == ('release', 'upload') else 90)

    def api(self, endpoint):
        return json.loads(self.call('api', endpoint))

    def release(self, tag):
        # GitHub's by-tag endpoint omits drafts, even for the authenticated owner.
        result = json.loads(self.call('release', 'view', tag, '--repo', REPO, '--json', 'apiUrl'))
        return self.api(result['apiUrl'])

    def upload(self, tag, path):
        expected_size, expected_hash = path.stat().st_size, hash_file(path)
        if expected_size >= 2 * 1024**3:
            raise RuntimeError('Release asset exceeds the GitHub 2 GiB limit')
        last_error = None
        for attempt in range(3):
            release = self.release(tag)
            existing = next((a for a in release['assets'] if a['name'] == path.name), None)
            if existing:
                if existing['size'] == expected_size and existing.get('digest') == 'sha256:' + expected_hash:
                    return existing, expected_hash
                raise RuntimeError(f'Existing asset differs; refusing to replace it: {path.name}')
            try:
                self.call('release', 'upload', tag, str(path), '--repo', REPO)
                asset = next(a for a in self.release(tag)['assets'] if a['name'] == path.name)
                if asset['state'] != 'uploaded' or asset['size'] != expected_size:
                    raise RuntimeError(f'Incomplete remote upload: {path.name}')
                if asset.get('digest') != 'sha256:' + expected_hash:
                    raise RuntimeError(f'Remote SHA-256 does not match: {path.name}')
                return asset, expected_hash
            except Exception as error:
                last_error = error
                emit(f'Upload attempt {attempt + 1} did not finish; retaining {path.name}')
                if attempt < 2:
                    time.sleep(2)
        raise last_error


def group_files(names, files):
    groups, current, size = [], [], 0
    for name in names:
        if files[name]['size'] >= LIMIT:
            raise RuntimeError(f'File exceeds this backup volume policy: {name}; preserve it and extend the policy')
        if current and size + files[name]['size'] > LIMIT:
            groups.append(current)
            current, size = [], 0
        current.append(name)
        size += files[name]['size']
    if current:
        groups.append(current)
    return groups


def write_volume(path, names, files):
    with zipfile.ZipFile(path, 'w', compression=zipfile.ZIP_DEFLATED, compresslevel=1, allowZip64=True) as archive:
        for name in names:
            original = files[name]
            source = ROOT / name
            digest = hashlib.sha256()
            info = zipfile.ZipInfo.from_file(source, name, strict_timestamps=False)
            info.compress_type = zipfile.ZIP_STORED if source.suffix.lower() in STORE_SUFFIXES else zipfile.ZIP_DEFLATED
            info._compresslevel = 1
            with source.open('rb') as incoming, archive.open(info, 'w', force_zip64=True) as outgoing:
                while chunk := incoming.read(BLOCK):
                    digest.update(chunk)
                    outgoing.write(chunk)
            after = source.stat()
            if (after.st_size, after.st_mtime_ns) != (original['size'], original['mtime_ns']):
                raise RuntimeError(f'File changed during snapshot: {name}; retry when the workspace is idle')
            original['sha256'] = digest.hexdigest()
    # Read the actual ZIP payload back, not only the in-memory source hash.
    with zipfile.ZipFile(path) as archive:
        for name in names:
            digest = hashlib.sha256()
            with archive.open(name) as stream:
                while chunk := stream.read(BLOCK):
                    digest.update(chunk)
            if digest.hexdigest() != files[name]['sha256']:
                raise RuntimeError(f'Local archive verification failed: {name}')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--branch', choices=['main', 'vulkan-testing'], default='vulkan-testing')
    parser.add_argument('--release')
    parser.add_argument('--full', action='store_true')
    parser.add_argument('--initial-release', action='store_true')
    args = parser.parse_args()
    STATE.mkdir(exist_ok=True)
    config = json.loads((ROOT / 'config/backup.json').read_text(encoding='utf-8-sig'))
    if not args.initial_release and not config['enabled']:
        emit('Backup is disabled; no action.')
        return
    marker = ROOT / config['stop_marker']
    if not args.initial_release and marker.exists() and json.loads(marker.read_text()).get('ready') is True:
        emit('Vulkan readiness marker is set; ongoing backups have finished.')
        return
    branch = run(['git', 'branch', '--show-current'])
    if branch != args.branch:
        raise RuntimeError(f'Expected branch {args.branch}, found {branch}; do not switch branches automatically')
    remote = run(['git', 'remote', 'get-url', 'origin'])
    if remote not in {f'https://github.com/{REPO}.git', f'git@github.com:{REPO}.git'}:
        raise RuntimeError('Unexpected backup remote')
    github = GitHub()
    files, directories, excluded = inventory()
    latest_path = STATE / 'latest-manifest.json'
    previous = json.loads(latest_path.read_text(encoding='utf-8')) if latest_path.exists() and not args.full else None
    old = previous['files'] if previous else {}
    changed = [name for name, record in files.items()
               if name not in old or record['size'] != old[name]['size'] or record['mtime_ns'] != old[name]['mtime_ns']]
    changed_set = set(changed)
    deleted = sorted(set(old) - set(files))
    source_changed = stage_source(files)
    if not changed and not deleted and not source_changed:
        # An earlier commit may still need a push after a network failure.
        run(['git', 'push', 'origin', args.branch], env=github.env)
        emit('No workspace changes; branch is backed up. No duplicate release created.')
        return
    if source_changed:
        title = 'Release 0.1: preserve Build 86 DX9 development checkpoint' if args.initial_release else 'Backup workspace source ' + datetime.now(timezone.utc).strftime('%Y-%m-%d %H:%M UTC')
        run(['git', 'commit', '-m', title])
    commit = run(['git', 'rev-parse', 'HEAD'])
    run(['git', 'push', '--set-upstream', 'origin', args.branch], env=github.env)
    tag = args.release or 'backup-' + datetime.now(timezone.utc).strftime('%Y%m%d-%H%M%S')
    local_tag = run(['git', 'tag', '--list', tag])
    if not local_tag:
        run(['git', 'tag', '-a', tag, '-m', 'Private workspace checkpoint ' + tag, commit])
    if run(['git', 'rev-list', '-n', '1', tag]) != commit:
        raise RuntimeError('Refusing to move an existing backup tag')
    run(['git', 'push', 'origin', tag], env=github.env)
    session = STATE / tag
    session.mkdir(exist_ok=True)
    try:
        release = github.release(tag)
    except RuntimeError as error:
        if '404' not in str(error) and 'release not found' not in str(error).lower():
            raise
        notes = ROOT / 'docs/RELEASE-0.1.md' if args.initial_release else session / 'release-notes.md'
        if not args.initial_release:
            notes.write_text(f'Private incremental workspace backup on `{args.branch}`.\n\nSource commit: `{commit}`.\n\n'
                             'Owned Steam COD4 content remains private. See snapshot-manifest.json and docs/BACKUP.md for restoration. '
                             'This backup does not qualify or implement Vulkan.\n', encoding='utf-8')
        github.call('release', 'create', tag, '--repo', REPO, '--verify-tag', '--draft', '--prerelease',
                    '--title', 'cod4x1khz 0.1 — Build 86 DX9 checkpoint' if args.initial_release else tag,
                    '--notes-file', str(notes))
        release = github.release(tag)
    if not release['draft']:
        raise RuntimeError('Snapshot releases must stay draft until all uploads are checked')
    manifest = {'schema': 1, 'repository': REPO, 'private': True, 'release_tag': tag,
                'source_commit': commit, 'branch': args.branch, 'full_snapshot': previous is None,
                'created_utc': datetime.now(timezone.utc).isoformat(), 'files': {}, 'directories': directories,
                'excluded': ['.git/** (root repository is preserved remotely)', '.backup/** (backup staging and bookkeeping)', *excluded],
                'deleted_since_previous': deleted, 'previous_release': previous['release_tag'] if previous else None,
                'volumes': []}
    for name in files:
        if name not in changed_set:
            manifest['files'][name] = old[name]
    groups = group_files(changed, files)
    progress_path = session / 'in-progress.json'
    emit(f'Backing up {len(files)} files; {len(changed)} changed; {len(groups)} volumes. Source {commit[:12]}.')
    for index, names in enumerate(groups, 1):
        archive_name = f'{tag}-workspace-{index:03d}.zip'
        archive_path = session / archive_name
        emit(f'Volume {index}/{len(groups)}: packing and checking {len(names)} files.')
        write_volume(archive_path, names, files)
        emit(f'Volume {index}/{len(groups)}: uploading {archive_path.stat().st_size / 1024**2:.1f} MiB.')
        asset, digest = github.upload(tag, archive_path)
        for name in names:
            manifest['files'][name] = {**files[name], 'archive_tag': tag, 'archive_name': archive_name}
        manifest['volumes'].append({'name': archive_name, 'size': asset['size'], 'sha256': digest,
                                    'asset_id': asset['id'], 'url': asset['browser_download_url']})
        atomic_json(progress_path, manifest)
        # Only remove this explicitly owned, checked staging file.
        if archive_path.resolve().parent != session.resolve():
            raise RuntimeError('Invalid local staging path')
        archive_path.unlink()
        emit(f'Volume {index}/{len(groups)} verified on GitHub; local staging freed.')
    # The source/workspace must remain unchanged while being captured.
    final_files, _, _ = inventory()
    if {name: (v['size'], v['mtime_ns']) for name, v in final_files.items()} != {name: (v['size'], v['mtime_ns']) for name, v in files.items()}:
        raise RuntimeError('Workspace changed during capture; draft retained. Retry when idle.')
    manifest['files'] = dict(sorted(manifest['files'].items()))
    manifest['total_files'] = len(manifest['files'])
    manifest['total_bytes'] = sum(v['size'] for v in manifest['files'].values())
    path = session / 'snapshot-manifest.json'
    atomic_json(path, manifest)
    _, manifest_hash = github.upload(tag, path)
    sums = session / 'SHA256SUMS.txt'
    sums.write_text(''.join(f"{v['sha256']}  {v['name']}\n" for v in manifest['volumes']) + f'{manifest_hash}  snapshot-manifest.json\n', encoding='utf-8')
    github.upload(tag, sums)
    github.call('release', 'edit', tag, '--repo', REPO, '--draft=false', '--prerelease', '--latest=false')
    finished = github.release(tag)
    if finished['draft']:
        raise RuntimeError('Release did not publish after verification')
    atomic_json(latest_path, manifest)
    atomic_json(session / 'verification.json', {'complete': True, 'repository': REPO, 'release': finished['html_url'],
                'files': manifest['total_files'], 'bytes': manifest['total_bytes'], 'volumes': len(groups),
                'source_commit': commit, 'manifest_sha256': manifest_hash, 'verified_utc': datetime.now(timezone.utc).isoformat()})
    emit(f"Complete: {finished['html_url']} — {manifest['total_files']} files, {manifest['total_bytes'] / 1024**3:.3f} GiB.")


def entrypoint():
    STATE.mkdir(exist_ok=True)
    lock = STATE / 'running.lock'
    try:
        descriptor = os.open(lock, os.O_CREAT | os.O_EXCL | os.O_WRONLY)
    except FileExistsError:
        pid = int(lock.read_text() or '0')
        if os.name == 'nt':
            handle = ctypes.windll.kernel32.OpenProcess(0x1000, False, pid)
            if handle:
                ctypes.windll.kernel32.CloseHandle(handle)
                emit('Another backup is running; no action.')
                return
        raise RuntimeError('A stale backup lock remains; inspect it before retrying')
    try:
        with os.fdopen(descriptor, 'w') as stream:
            stream.write(str(os.getpid()))
        main()
    finally:
        lock.unlink(missing_ok=True)


if __name__ == '__main__':
    try:
        entrypoint()
    except Exception as error:
        print(f'Backup failed: {error}', file=sys.stderr)
        sys.exit(1)
