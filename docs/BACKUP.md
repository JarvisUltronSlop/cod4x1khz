# Source and local backups

The public `JarvisUltronSlop/cod4x1khz` repository contains the edited engine,
pinned reference source snapshots, configuration, SDK, tests, scripts,
documentation and original license notices. Retail game assets, original game
executables, proprietary middleware binaries, player profiles and workspace
backup archives are not distributed through this repository or its releases.

Release **0.1** is the historical Win32/DX9 source checkpoint. Release **0.2**
contains the native x64 installer, its checksum file and a package manifest.
The installer requires each user's owned Steam installation and copies the
necessary retail assets locally. See [installation](INSTALLER.md).

The source history was cleaned before public publication on October 29, 2026.
Commit IDs after the initial checkpoint changed as a result. Existing private
clones must not force-push their old history into this public repository; clone
the public repository afresh when moving a separate checkout to public work.
The original private repository and recovery history remain in a private archive.

Release-based workspace backups are disabled in `config/backup.json`.
`./backup.ps1` exits with **Backup is disabled; no action.** before Git or network
operations. The historical backup uploader also requires a private repository.
Do not enable it against the public repository.

Keep personal recovery copies of player profiles and server profiles in private
storage. Never attach a full installed output folder or a development workspace
archive to a public release: both can contain copied retail game data and private
server configuration. Source-only changes belong in normal Git commits.
