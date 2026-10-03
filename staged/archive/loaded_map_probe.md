# Loaded-map archive codec probe proposal

This is an unapplied fixture contract. Build 73 contains the production archive adapter, but this probe has not been compiled, linked or run. The completed offline tests in `results/20261003-122302-671923/result.json` exercise the policy and staged adapter with synthetic dependencies. They do not execute `SV_ArchiveSnapshot` or its native decoder.

## Smallest safe boundary

Use one synchronous, authenticated, benchmark-only callback on the dedicated main owner. It does not need the ALL14 checkpoint coordinator or a persistent host pause: the live game world is held stationary while this callback runs, and is never captured or rewound. The callback may temporarily exercise the archive's own clock and storage, then restore all codec storage before returning.

Required entry checks are actual main-thread identity, dedicated custom build, `khz_benchmark` and `khz_test_controls`, a loaded `SS_GAME` map, `!com_inServerFrame`, `!sv.inFrame`, `!svsHeaderValid`, `!g_archivingSnapshot`, no active archive attempt, matching native counters/sidecar index, idle VM, completed asset loading, no loaded plugin, and no active web/content/screenshot transfer or worker callback. Reject a persistent world checkpoint pin rather than modifying a captured world behind that coordinator. Reject recursion and a pending map/killServer/error transition.

The callback must not run `Com_EventLoop`, `Cbuf_Execute`, `KHZ_HostPoll`, `Steam_CheckClients`, script callbacks, packet send/receive, asset load/wait helpers or service callbacks. It must not wait for a worker or message pump. Allocate its bounded backup and result storage before any codec mutation. Queued UDP datagrams and host commands remain queued until the callback returns.

The inspected multiplayer paths support this boundary:

- `server_mp/sv_main_mp.cpp:1232`: `SV_FrameInternal` advances the world synchronously and asserts the main thread.
- `server_mp/sv_init_mp.cpp:969`: `SV_CheckThread` requires main. The separate `SV_InitServerThread` startup call in `qcommon/common.cpp:1487` belongs to `KISAK_SP`.
- `qcommon/common.cpp:998`: `Com_ServerPacketEvent` owns real and loopback packet dequeue and `SV_PacketEvent` dispatch on main.
- `server_mp/sv_snapshot_mp.cpp:1792`, `:1827`, `:1987`: the header, cache and archive writes are synchronous owner calls.
- `win32/win_steam.cpp:93`: the Steam callback adds an ID to its synchronized queue. Main `Steam_CheckClients` performs actual client drops.
- `khz_host.cpp`: its pipe worker queues controls; gameplay mutation happens only when the owner drains them.

No independent writer to archive/cache arrays was found in those paths. Asset database loading and optional plugin/service workers remain explicit exclusions. This statement is scoped to the dedicated custom multiplayer build and these native call paths.

An existing private host command or authenticated localhost RCON callback can request the probe while the world is ordinarily active, provided it dispatches only at the completed boundary above. The earlier persistent pause proposal is unnecessary for this synchronous fixture and would complicate command routing: paused UDP is discarded and `HostMessage::Command` intentionally refuses that state.

## Exact codec backup

Save original arena bytes, not just their counters. Restore to the same original allocations in this process. Do not copy all of `svs`, client netchannels, script heaps, the hunk or physics arenas and call that a world checkpoint.

| Native storage | Capture and restore |
| --- | --- |
| `svs.cachedSnapshotFrames[512]` | All native descriptors, including writer-created full bases and decoder metadata. |
| `svs.cachedSnapshotEntities[16384]` | Entire entity payload ring. |
| `svs.cachedSnapshotClients[4096]` | Entire PS/client-state payload ring. |
| `svs.archivedSnapshotFrames[1200]` | All typed `{start,size}` descriptors. |
| `svs.archivedSnapshotBuffer[33554432]` | All 32 MiB bytes, including overwritten/previously unused portions. |
| Archive/cache counters | `nextCachedSnapshotFrames`, `nextCachedSnapshotEntities`, `nextCachedSnapshotClients`, `nextArchivedSnapshotFrames`, `nextArchivedSnapshotBuffer`. |
| Codec time | Original `svs.time`, if temporary controlled codec sample times are used. `level.time`, gameplay tick, command timeline and scripts never advance in the callback. |
| Header and message scratch | `svsHeader`, `svsHeaderValid`, actual `g_archiveMsg`, all `tempServerMsgBuf[131072]`, and `g_archivingSnapshot`. |
| Adapter metadata | Actual private `archivePolicy`, its epoch/tags, deadline, bitmap, active attempt and diagnostic counters. |
| Native net-field statistics | Actual `netFieldOrderInfo_t orderInfo` from `qcommon/sv_msg_write_mp.cpp:13`. |
| Snapshot profiling scratch | All mutable globals declared/defined in `server_mp/sv_snapshot_profile_mp.cpp`, listed below. |

`cachedClient_s` is 0x2FCC bytes, so its 4,096-row ring alone is roughly 48 MiB. Use a one-time bounded heap image or preallocated arena; do not initialize or copy this image through an automatic stack temporary. Restore raw bytes only to these original trivially stored rings. No C++ object, vector, OS handle or foreign allocation ownership is transferred.

Snapshot profile backup is source-owned at the end of `sv_snapshot_profile_mp.cpp`. It must include `g_bitsSent`, `s_totalPacketDataSizes`, `s_packetMetaDataSize`, `s_packetModeStart`, `s_packetMode`, `g_currentSnapshotPerEntity`, `g_currentSnapshotFieldsPerEntity`, `g_currentSnapshotPlayerStateFields`, `newDataReady`, `bitsUsedPerEType`, `bitsUsedForPlayerstates`, `playerStateFieldsChanged`, `s_packetDataEnabled`, `s_floatBitsCompressed`, both origin delta arrays, both origin full arrays, `networkEntityFieldsChanged`, `currentSnapshotNetworkEntityFieldsChanged`, `bitsUsedForServerCommands`, `s_currentEntType`, `s_currentEntNum`, both origin-error counters, `s_stdSnapshotDeviation`, `s_avgSnapshotSize`, `s_maxSnapshotSize`, `s_numSnapshotSamples`, `s_numSnapshotsBuiltSinceLastPoll`, `s_uncompressedDataSinceLastPoll`, `s_compressedDataSinceLastPoll`, and `s_numSnapshotsSentSinceLastPoll`. Immutable names/tables are not restored. `g_archivingSnapshot` is included in the codec image above and has one restore owner.

Disabling packet-data recording is insufficient: `SV_ResetPacketData` changes its mode/start unconditionally; EType/PS/field counters and `orderInfo` are also incremented without that flag. The snapshot profile source contains `s_avgSnapshotSize`, which the public header does not declare; use its source-owned fragment instead of inventing an external definition.

Require the actual MSG Huffman initializer to have completed before capture. The probe uses the raw archive codec, not packet compression; it must not initialize or restore `std::once_flag`, the immutable Huffman codes or its heap/tree ownership. Reject network debug/spew dvars that would produce additional external output during fixture encoding/decoding.

The fixture does not call `SV_EndClientSnapshot`, packet compression or `SV_SendClientMessages`; those would touch actual client transfer history, packet counters and external sockets beyond this backup scope. Invoke the archive/header calls directly.

## Proposed source-owned API

An EOF fragment in `khz_archive.cpp` can access the actual private sidecar without a generic production mutation API. A profile fragment in `sv_snapshot_profile_mp.cpp` owns the profile image. Export only a synchronous diagnostic runner and bounded completed JSON getter, for example:

```cpp
bool KHZ_ArchiveRunCodecProbe();       // completed dedicated owner only
const char* KHZ_ArchiveCodecProbeJSON();
```

The runner owns backup, scenario execution and unconditional restore. All saved tuple/address checks occur before the first mutation; the exit path verifies exact restored bytes and values, rather than simply setting a `restored=true` flag. Probe results include original session/map/native time, full and delta counts, exact serialized comparisons, split/overflow/headroom cases, cleanup completeness, and explicit `full_world_restore=false` / `authoritative_replay=false`. The callback is excluded from steady tick/FPS acceptance measurements.

For controlled scenario construction, the fragment may create a temporary empty archive policy at the original epoch and clear only the codec counters/rings in the backed-up fixture branch. It must not invoke a map-lifecycle reset or increment the real map epoch. Ordinary public `KHZ_ArchiveRestoreSidecar` is not a standalone world rewind API; the fixture restores its own original codec image and private policy under the same synchronous scope.

## Native scenarios

1. **Real full and delta.** Read real player/client/entity sources through the native header and encoder on the loaded map. Sample the unchanged actual world at two controlled codec times 50 ms apart. Force the first record to be full and require the second to be a delta from that committed full. Force cache misses so both native decode paths execute; writer cache hits alone are not roundtrip proof. Compare all decoded serialized client/PS/entity fields between the full and unchanged-world delta, plus their actual committed times/kinds/base identity.

   Native serialization quantizes some values and does not transmit every byte of `playerState_s`. Expected results must follow native field definitions; a whole-PS `memcmp` against the live world is not a correct oracle. `GetFollowPlayerStateLocal` intentionally clears `hud.current`; compare that archived representation. Archived PS sets the send-origin flag, so the native reader must not invoke its nonarchive `CL_GetPredictedOriginForServerTime` branch.

2. **Logical delay.** With two or more actual committed records, vary only fixture codec `svs.time` through the 50 one-millisecond remainders. A normal ceiling keeps the original request, an older-than-history request clamps only to oldest available age, and current/no-history fallback returns zero. This tests lookup; it does not establish actual spectator/killcam behavior in a live match.

3. **Actual MSG overflow and reused id.** Before a required full encode, use bounded real `MSG_WriteData` writes to fill the actual 131,072-byte message close to capacity. Run the real encoder with unchanged capacity and backing storage, observe its overflow and actual cache payload/counter writes, then call the production completion path. Raw bytes/descriptor/id must remain uncommitted while native cache counters remain advanced. The next successful encode at a later controlled sample time reuses the id. Reject the earlier phantom full base/cached row and compare real decoded payload values after reuse. Do not reduce and later falsify `msg.maxsize` or roll back counters after overflow.

4. **Borrowed full base headroom.** Place an actual captured full-cache descriptor at the next descriptor write position, or move a copy of its actual client rows to the precisely constructed eviction edge, inside the backed-up codec branch. Update its real cache metadata/counters coherently. Query a real committed delta so the production guard forces a fresh full decode from raw bytes. Compare the full decoded serialized state before/after and the delta output; do not assert success solely from a guard return or counter change.

5. **Actual 32 MiB split.** Use real encoded records and bounded trailing MSG padding after their end marker to position committed bytes near the ring boundary. Trailing padding is explicitly synthetic fixture construction. Encode and commit a message across the tail, force its cache miss, and require native `MSG_InitReadOnlySplit` decoding to reproduce the same serialized state. Expire a raw full-base extent with further actual committed writes and verify cached presence cannot resurrect its dependent delta. Descriptor wrap is a separate bounded scenario and must keep exact id/tag/time ordering.

Do not corrupt committed kind/time/base bytes and invoke the native decoder in the first fixture. Those are existing policy/adapter negative contracts; actual malformed native payload decoding is an additional fixture requiring a scoped native error boundary.

## Error and cleanup boundary

Native `Com_Error` longjmps through main TLS and bypasses C++ destructors. The ordinary shutdown hook correctly invalidates abandoned archive flags after server teardown, but it does not restore a temporary probe image. A loaded-map runner must not rely only on RAII for cleanup.

Before applying the probe, provide one of these concrete policies: a narrowly scoped main-owner error interception that restores the codec/profile image before rethrowing the original native error, or a source-owned ordinary error-cleanup hook that discards the saved image before native server/map storage is released and reports an explicit failed/terminated fixture. Never copy saved bytes back after the server lifetime has been destroyed. The first option permits an exact codec cleanup result; the second honestly fails and lets the harness verify owned-process cleanup. Active Tracy zones need their own unwind treatment; refuse the probe in a Tracy-enabled build unless that scope has been handled.

At normal exit, first close any active header/archiving/attempt state within the fixture branch, then restore payload/descriptor arrays and counters, native time and message/header scratch, actual private policy and statistics. Finally compare the restored image and clear the probe-active marker. No native callbacks, setters or allocations occur during restore. Resume ordinary processing only after complete successful cleanup. Pending datagrams may then be consumed at their original normal owner boundary.

## Remaining evidence

This fixture would prove real codec paths and their scoped cleanup only. Live killcam/follow delay, death/respawn/admission semantics, the custom 250 ms 1 kHz rewind history, map/error lifetime behavior, all-domain authoritative replay, and actual four-case 60-second performance measurements remain separate gates.
