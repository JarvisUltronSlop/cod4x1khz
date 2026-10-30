# Private backups and recovery

Repository: `JarvisUltronSlop/cod4x1khz`. The initial source checkpoint is on
`main`, tagged **v0.1**. Release **v0.2** on `vulkan-testing` contains the native
x64 game, Vulkan/DX9 renderers, installer and Server Manager.

On October 29, 2026, the owner requested only releases 0.1 and 0.2 on the releases
page. The ten `backup-*` releases, including the unfinished draft, are being
removed as part of that publication. Release-based backups are disabled in
`config/backup.json` so scheduled invocations cannot recreate them. The v0.1
release and its recovery assets are retained, as are source commits, Git tags
and local workspace files. Historical incremental manifests referencing removed
release assets no longer provide a complete downloadable recovery set.

## What is saved

Git tracks the complete edited engine, pinned reference source snapshots,
configuration, SDK, tests, scripts, documentation and original license notices.
Nested upstream repositories are tracked as ordinary source files, not Git links.

The **private v0.1 release** additionally holds independent ZIP volumes and
`snapshot-manifest.json`. The manifest records every captured file's path,
size, SHA-256, modification time and containing release/archive. This includes
current game assets, profiles, dependencies, startup DLLs, compiled builds,
historical results/logs, staged work and original nested Git metadata. Root Git
metadata is recovered from GitHub. Backup staging/bookkeeping (`.backup/`) and
credential/key files are excluded explicitly in the manifest. Empty directory
names are recorded; NTFS ACLs and alternate data streams are not archived.

Volumes are at most 512 MiB of uncompressed source data and individually below
GitHub's 2 GiB asset limit. ZIP payloads are read back and checked against file
hashes before upload. GitHub's asset size and SHA-256 are checked afterward.
Only verified staging ZIPs are removed locally to conserve disk space. The
workspace's original files are preserved.

This contains the owner's Steam game data and proprietary runtime dependencies.
**Keep this repository and its backup releases private.** The archives are not
redistributable game packages. A future public source release requires a separate
content/license review and must continue to require each user's owned game.

## Historical backup mechanism (disabled)

```powershell
./backup.ps1
```

The command now exits with "Backup is disabled; no action." The following
describes the former mechanism, not a request to resume release uploads.
When enabled on `vulkan-testing`, the script verifies the account, private repository,
remote and current branch, stages complete source files, commits changes and
pushes without force. Changed workspace files go into a timestamped private
prerelease. Unchanged files reference already checked older volumes, so unchanged
retail assets are not uploaded every hour. Every manifest describes the complete
workspace, including deletions and its previous checkpoint.

A quiet hourly task was configured to run this command when files changed,
skipping active game/test/build processes. With the backup setting disabled,
any remaining scheduled invocation exits before staging, pushing or uploading.

Set `config/backup.json`'s `enabled` to false or disable the task in Scheduled
to stop it. When Vulkan readiness is actually confirmed, the documented
`docs/vulkan-ready.json` marker also stops backup work. Source commit messages
describe checkpoints; a backup is not an acceptance or parity claim.

Interrupted uploads stay draft. Original files and unverified staging volumes
remain local. Retry the same tag when idle:

```powershell
./backup.ps1 -ReleaseTag backup-YYYYMMDD-HHMMSS
```

Do not move v0.1 or overwrite a verified remote asset. If a draft's source commit
differs from the current working snapshot, preserve that draft and use a new tag.

## Restore a checkpoint

Install Git, Python 3 and the GitHub CLI; authenticate with an account that can
read this private repository. Clone the source to obtain recovery tooling:

```powershell
git clone https://github.com/JarvisUltronSlop/cod4x1khz.git recovery-tools
Set-Location recovery-tools
gh release download v0.1 --repo JarvisUltronSlop/cod4x1khz --pattern snapshot-manifest.json --dir downloaded
python scripts/restore_github_backup.py --manifest downloaded/snapshot-manifest.json --destination C:\Recovery\cod4x1khz
```

The destination must be new or empty. The restore tool downloads referenced
volumes one at a time, checks archive digests and file payload hashes, restores
modification times, then frees only its downloaded staging volumes. It never
extracts unrequested entries, follows unsafe archive paths, or overwrites an
existing project. The retained v0.1 manifest references its original recovery
volumes. Removed incremental releases cannot be restored through their former
download URLs.

Alternatively, download the manifest and all referenced volumes through GitHub's
release pages, then pass `--archives C:\Recovery\downloaded` to avoid API downloads.
Keep enough free space for the restored workspace plus one staging volume.

After recovery, use GitHub to clone/check out the manifest's `source_commit` if
you need root Git history, or initialize a root repository and attach the original
remote. Original nested checkouts are included in the personal snapshot. Fresh
source-only clones can instead use `prepare.ps1` with their own Steam assets;
see [BUILD.md](BUILD.md).
