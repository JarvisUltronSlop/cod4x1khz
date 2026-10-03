# Prepared integration, not applied

`prepare_production_integration.py` prepares a unified review patch and exact original-source checksum plan entirely inside `staged/archive`. It has no apply, build or game-launch option. It is not run during exclusive live measurements. Root must review and authorize production application, compile/link both targets and archive actual source/binary provenance before the loaded-map fixture. None of these new boundary fragments has received native type/link or live proof yet; the earlier probe/profile type check does not cover these new helpers.

The proposal uses actual dedicated custom guards (`COD4X1KHZ && KISAK_MP && KISAK_DEDICATED`), twelve source-owned EOF fragments/headers and small native entry hooks. There is no independently compiled extra copy of a native owner TU, no CMake implementation source added, no server-checkpoint option enabled and no domain mask reduced. The real codec and real profiler fragments remain at the ends of their original owners.

## Origin and lifecycle

Only an exact direct `khz_archive_probe` packet establishes the private marker. All other already-valid `HostMessage::Command` packets return from the added begin helper without a marker or new readiness checks; their ordinary behavior is preserved. The exact probe marker is bound to the authenticated private pipe's actual parent PID, token comparison, accepted sequence, server session, logical tick and server ID. An ordinary RCON, script, nested `exec`/`vstr` or another command cannot acquire this marker. The handler checks it again before invoking the codec, and the native entry callback checks it before allocating/changing codec state.

`KHZ_InitArchiveCodecProbeCommand()` installs the actual boundary callback before exposing the native command. It is called at the end of `KHZ_Init`, after benchmark/test-control dvars have their protected captured startup values. The owned-console allowlist gains only this command; public RCON remains unable to execute the codec because command visibility is not execution permission. The command sends a bounded typed origin/result envelope through the existing 2048-byte private reply. It refuses oversized JSON rather than sending a truncated successful record. It records actual synchronous owner wall milliseconds and preserves the exact native failure reason.

Normal return explicitly clears the host marker. An abandoned redirect scope must not retain permission: the proposal's first `Com_ErrorCleanup` action restores/frees a live codec image and clears the marker before bot/admin/plugin/service cleanup. `SV_Shutdown` repeats these idempotent actions at its beginning, before `Com_SyncThreads`, `SV_FinalMessage` or any world/map/arena release. The original native error continues. This is explicit native longjmp cleanup, not RAII. A fatal process exit that does not return cannot produce a passing fixture; normal OS teardown frees its process allocations. The first fixture intentionally injects no native errors. Tracy-enabled native builds remain refused.

## Typed owner checks

| Original owner | Actual checked state | First-scope exclusion |
| --- | --- | --- |
| `khz_host.cpp` | private authenticated scope, PID/sequence/session/tick/server-ID, phase, pending request/ready/quit, checkpoint state | wrong origin, pending map/stop/fence or changed lifetime |
| `khz_plugins.cpp` / `plugin_registry.hpp` | persistent `generation_` through new const `LoadGeneration()`, callbacks, modules/import pins, atomic job phases, service queues | **any load attempt ever admitted to the registry**, even after unload/failure; all jobs/callbacks |
| `khz_content.cpp` | atomic never-cleared worker-start marker, actual worker/running plus typed `GetProgress`, request/completion ring cursors | any worker-start attempt ever, pending transfer/completion |
| `khz_content_server.cpp` | never-cleared owner start marker, actual `std::thread`, accepted requests, job/result ring cursors, deferred errors | any hash worker-start attempt ever or metadata service request history |
| `khz_screenshot_server.cpp` | actual pending cursors, stopping atomic and all four atomic slot phases | a queued transfer or any receiving/verifying/result/discard task; normal idle worker permitted |
| `khz_webadmin.cpp` | actual constructed thread/running publication, all atomic request slot phases | starting/running web listener or retained request |
| `khz_admin_server.cpp` after web adapter | actual request/plugin-ban/audit queues, native invoker/redirect/remote-console context, actual enabled web dvar | pending admin/web transition/service context |
| `qcommon/cmd.cpp` | actual client/server native command lengths, `cmd_wait`, native Cbuf execution flags | any pending text or Cbuf execution; no string-based classification |

Foreign callbacks cannot pass an owner-field check: every helper first requires `Sys_IsMainThread`. Plugin history is checked before content or plugin queues are read; a persistent registry generation of zero proves no module ever obtained SDK callbacks/worker permission. The registry's per-slot generation is insufficient because `FinishUnload` clears it. The proposed accessor does not modify registry behavior.

The ordinary screenshot worker is initialized for a fresh dedicated server, so merely detecting its existence would reject every valid fixture. Atomic `Free` is published after its task's writes finish; with every phase free and the owner pending queue empty, no screenshot worker can acquire work while the synchronous main callback runs. The helper never reads mutable payload buffers. The web guard rejects a constructed thread even in its pre-`running` startup gap.

Content workers differ. `KHZ_InitServicesClientFeatures` starts `khz::content` automatically for the **client**, but the dedicated `KHZ_Init` excludes that call under `DEDICATED`. The only actual `content::Start` caller is that services initializer. Dedicated `KHZ_ContentServerInit` initializes request state without a worker; the hash worker starts only after an accepted metadata request is resolved during outer service. A fully installed stock fixture therefore has a viable never-started child boundary. If actual startup downloads/metadata start either child worker, the probe must report an explicit refusal; these exclusions are not silently waived for convenience. The producer start markers are set before thread construction and never reset on error, shutdown or map changes, conservatively rejecting even failed construction attempts.

The native probe separately requires completed `SS_GAME` owner/frame/header/VM boundaries, database ready, valid original archive/cache identities, no native download/WWW/netchan fragments and no native codec debug mode. It leaves datagrams queued and does not pump event loops, Cbuf, host controls, services, Steam or DB work. Any persistent all-domain server checkpoint pin is rejected when that option exists; the proposal does not activate that feature. A missing boundary callback rejects before mutation.

## Unlaunched loaded-map fixture

After root has applied/linked/archived the integration and granted an exclusive game slot:

```powershell
python staged/archive/native_codec_probe.py --build-record results/build74-source-and-binary-hashes.json --client-port 28991 --budget 90
```

The actual client adapter is `khz_ui server khz_archive_probe` → `KHZ_HostConsoleCommand` → authenticated private `HostMessage::Command`; no `hostcmd` console command currently exists. The fixture reuses the source-reviewed `ManagedHostFixture` to start the actual root executable from unrelated `C:\Windows\Temp`, preserve/isolate its benchmark home, start one human plus15 Regular bots on Killhouse/DM, and retain the exact owned child handle/path. It requires archived client/server binary hashes and actual native readiness/admitted input acknowledgement before probing.

First it retains an actual public RCON refusal datagram. Then it captures a fresh exact-client log byte offset, queues one owned probe and requires one complete typed reply with the actual parent/child PID, sequence/session/tick/server-ID identity. All real codec scenario flags, native decode/serialized/float counts, actual16-client/linked-entity population, exact scope cleanup and unchanged-world observations are required. Refusals and `[output truncated]` fail explicitly. Exact native reply bytes, hashes, full logs and source/binary provenance are retained.

The native wall-ms and caller queue-to-reply elapsed time describe an intentional diagnostic stall. The fixture also preserves actual post-return client raw input/snapshot/event queues, command publication cursors, client/server debt samples and live admitted commands/snapshots/pose/GPU progress. It requires recovery without a changed owner/map lifetime and no growing sampled debt. It performs native Leave, waits for stock frontend/owned-child termination, then Quit; forced termination fails the fixture. No steady timing acceptance may overlap this diagnostic. Full-world restore, authoritative replay, complete killcam gameplay and timing acceptance remain false.
