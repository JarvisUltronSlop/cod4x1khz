# 50 ms archive proposal and staged evidence

These files preserve the reviewed proposal. The root agent integrated a separate production copy as native/archive_history.hpp and engine/src/khz/khz_archive.{h,cpp} for Build 73. The staged adapter remains unchanged; it is not linked into the game. The isolated policy and synthetic native-state adapter contracts passed on 2026-10-03. Actual loaded-map native codec, killcam and timing evidence remain separate requirements.

The independent archive samples completed authoritative state every 50 ms. The first valid SS_GAME dispatch samples immediately; later dispatches use absolute native millisecond deadlines. A delayed dispatch captures one actual current state and skips expired recording opportunities. It does not synthesize historical worlds. The 1 ms authoritative ticks, human command/snapshot transmission, sv_fps/svsHeader.fps=1000, and the separate 250 ms KHZ_RewindClient history remain intact.

## Draft components

- archive_policy.hpp: fixed 1,200-row committed-id/time sidecar, absolute recording clock, full-base validation, circular availability bitmap, timestamp binary lookup, and decode storage headroom policy. Metadata is statically bounded below 80 KiB and trivially copyable.
- native_adapter.h/.cpp: native server storage/time adapter, completed-message commit, actual decoded-cache identity checks, logical-delay lookup, legacy position bracketing, and same-process checkpoint sidecar API.
- archive_policy_tests.cpp: executed policy contracts. These do not stand in for the native codec/world tests below.
- native_adapter_tests.cpp: the actual staged adapter compiled against native types, with synthetic owner/current-state/decoder/error dependencies. Seven groups and 131,533 comparisons passed. This exercises adapter metadata, ring copy, interpolation and sidecar bodies; it does not execute the native archive codec or restore a loaded world.
- run_contracts.py and CMakeLists.txt: isolated project-local MSVC Win32 Release configure/build/run and provenance capture. No game target or deployment is performed.

The production copy uses the destinations above and keeps noncustom native branches intact. No new protocol identifier or client transport change is needed. The hook list below records the proposal contract; use the actual production source and Build 73 archive to review the implemented hook locations.

## Required native hooks

All of these hooks belong to one implementation change. Gating recording alone would leave the native frame-count lookup and delta window incorrect.

1. Include the adapter declaration in server_mp/sv_archive_mp.cpp, server_mp/sv_snapshot_mp.cpp and server_mp/sv_init_mp.cpp under COD4X1KHZ.
2. At the end of SV_InitArchivedSnapshot in sv_init_mp.cpp:312, after its native archive/cache counters have reset, call KHZ_ArchiveResetPolicy. This covers map spawn and the actual fast-restart call in sv_ccmds_mp.cpp:528. Reset requires the completed server owner, svsHeaderValid=false, g_archivingSnapshot=false and native archive counters=0. Also call KHZ_ArchiveShutdownComplete at the end of the actual running-server SV_Shutdown branch, after SV_ClearServer, native svs zeroing and bgs clearing. Native error handling uses longjmp, so this completed shutdown hook explicitly invalidates abandoned header/archiving/attempt flags after the original server storage and active frame have been cleared. It never imports abandoned cached counters. No live frame or checkpoint-pinned map may reset this lifetime; existing pre-effect lifecycle guards must refuse shutdown/map mutation before teardown while pinned.
3. In SV_SendClientMessages in sv_snapshot_mp.cpp:1920, retain both human-client loops, packet construction, voice work and SV_SetServerStaticHeader. Replace only the final archive block:

    - Disable packet-data recording as before.
    - While the static header is valid, obtain archiveDue = SS_GAME && KHZ_ArchiveSampleDue().
    - Set g_archivingSnapshot to archiveDue. If due, initialize the actual g_archiveMsg with actual tempServerMsgBuf, capacity 131,072, and call SV_ArchiveSnapshot.
    - Call SV_GetServerStaticHeader unconditionally, including the 49 nondue dispatches. This closes the header and imports native cached-counter changes made by the encoder.
    - If due, call KHZ_ArchiveCompleteMessage(&g_archiveMsg). This replaces the old byte/descriptor/counter append block.
    - Clear g_archivingSnapshot and restore optional packet-data recording as before.

   Do not return early from a nondue archive branch. Do not bypass the 1 ms snapshot work or change svsHeader.fps.

4. In SV_ArchiveSnapshot in sv_archive_mp.cpp:72, replace the frame-count full-base predicate with KHZ_ArchiveCanUseDeltaBase(v36). That helper checks actual cache storage/ranges, a committed full descriptor, exact native time and timestamp age <=1,000 ms. The native full-cache search and client/entity serialization remain in their original order. An older decoded full base can remain eligible; the bitmap handles any resulting nonmonotonic availability exactly.
5. Immediately before the selected delta branch's MSG_WriteBit0, call KHZ_ArchiveEncodedDelta(v36). Immediately before the full branch's MSG_WriteBit1 at approximately line212, call KHZ_ArchiveEncodedFull(). Each actual attempt selects its kind once.
6. Route the custom SV_GetCachedSnapshot(int*) at sv_snapshot_mp.cpp:693 to KHZ_ArchiveLookup; preserve the noncustom body. Route the custom legacy SV_GetClientPositionAtTime at :527 to KHZ_ArchiveClientPosition. This legacy helper is separate from custom G_AntiLagRewindClientPos, which already calls KHZ_RewindClient.
7. At the start of SV_GetCachedSnapshotInternal at :728, before any modulo/descriptor indexing, return null if KHZ_ArchiveCommittedDescriptor(archivedFrame) rejects it. This rejects negative, future, overwritten, expired-base and uncommitted ids.
8. Its cached-hit search at :758 must require KHZ_ArchiveCachedFrameMatches(cachedFrame) in addition to the requested archived id. A rejected phantom/stale cache row must continue the search rather than terminate it. The helper verifies committed id/time/kind, actual cache ranges and the write headroom described below.
9. Replace all three decompiler size expressions at :772/:778/:783 with the typed svs.archivedSnapshotFrames[archivedFrame % 1200].size. The expression was a byte-array pointer crossing into its immediately preceding descriptor array: 1,200 * 8=9,600 bytes, and the size member is +4, hence -9,596. The typed member read removes that cross-array pointer arithmetic.
10. Read the leading bit into a local full/delta flag. Before creating or overwriting cached metadata, reject a message read overflow or mismatch with KHZ_ArchiveDecodedKindMatches(archivedFrame, !full).
11. In the full branch, read native time into v6 first; reject overflow or KHZ_ArchiveDecodedTimeMatches mismatch before assigning a new cached-frame descriptor or appending payload rows.
12. In the delta branch, read oldArchivedFrame and time first. Reject read overflow, KHZ_ArchiveDeltaReferenceMatches mismatch or KHZ_ArchiveDecodedTimeMatches mismatch before recursion or cache writes. The sidecar requires a strictly earlier committed full base with matching time, so self references, future references and delta chains never recurse. After the recursive full-base read, require its actual headroom-valid KHZ_ArchiveCachedFrameMatches and usesDelta=0 again before allocating the delta result. Move the existing later MSG_ReadLong into the early header read, preserving the exact wire field order.
13. Keep native decode error handling. Check final message overflow before promoting a decoded cache frame. A malformed message is an explicit native error; no partial cache counter rollback is permitted.

## Overflow and borrowed cache storage

SV_ArchiveSnapshot's full path writes cache clients/entities and advances native cache counters before the final message fit is known. On overflow the draft advances the archive clock, records an overflow, and leaves those actual cache writes/counters intact. It does not append bytes, commit a descriptor, advance native archive ids/byte counts, or change the archive availability bitmap. The next successful frame can reuse the same archive id with a different actual time. Exact committed time/kind checks reject the earlier phantom cache row.

Counter-only rollback would expose overwritten client/entity data as valid historical rows. This proposal never rewinds cache counters after overflow.

Delta decoding has another borrowed-storage hazard: its oldCachedFrame can occupy the next cache descriptor write slot, or its cached-client range can overlap the next 64 client-row writes. A cached hit is admitted only if it has one descriptor write slot and 64 client rows of headroom. Rejection forces the original full payload to be decoded again from its committed bytes. A fresh full frame is in a preceding descriptor slot with fresh client rows, so the immediately following delta write can consume it safely. The 64-row bound is the actual native client capacity, not the 16-player fixture count. Verify native client decode count/order bounds under the codec fixture; malformed payloads must fail before exceeding this bound.

Headroom changes cache use, not historical eligibility: a record is publicly available only when its own committed bytes and, for a delta, its committed full base bytes remain retained. The existence of a previously decoded cache frame never makes an evicted raw dependency readable.

## Lookup and retention

Successful archive ids remain dense; overflow omits a sample, not an id. Their actual times strictly increase. The sidecar rebuilds a 1,200-bit availability mask only after a successful archive commit, using native descriptor/byte retention and exact full-base dependencies. It handles arbitrary availability holes, including a later delta that chose an older full base. A lookup binary-searches actual committed timestamps and finds the next set bit in at most 20 circular bitmap words per search. The normal path uses about 11 timestamp comparisons, plus short bitmap searches; it does not scan 1,200 native rows on each 1 ms killcam request. A stalled clock may require a second timestamp search for the 60-second retention limit.

The selected record is the earliest available actual timestamp >= currentTime - requestedDelay. On ordinary ceiling rounding and internal unavailable gaps, the returned logical delay remains exactly the request. SV_BuildClientSnapshot at :1032 stores this value back into sess.archiveTime. Replacing it with the selected record's age every tick would erode a requested 5,000 ms delay to 4,951, then 4,902, and continue toward zero.

Only a request older than the oldest actually available record is clamped to that oldest age. If no usable ceiling sample exists, or its actual time equals the current time, the native current-client fallback receives delay0. Native PS/HUD/entity timestamp rebasing continues to use cachedFrame.time, not the preserved logical request.

The descriptor window nominally spans about 60 seconds at 20 recordings/s. There is no guaranteed 60-second horizon: the fixed 32 MiB byte ring may evict sooner, full-base retention can remove dependent deltas, and recording stalls/overflows can leave gaps. The explicit oldest-availability clamp reflects those actual limits.

The legacy position helper searches actual timestamp brackets with the current authoritative position as the newest anchor. It retains the original health/player/follow guards and 400 ms request limit. It can consume one sample up to 450 ms old to bracket an age400 request. It copies each origin before further decoding can evict the borrowed cache payload, and uses floating-point interpolation rather than the native integer division at :607. This is not proof of historical pose, respawn/admission interpolation correctness, or a replacement for the separate 1 ms custom rewind.

## Whole-checkpoint sidecar

Network part0 already captures/restores actual svs, including archive bytes, descriptors, decoded-cache payloads and counters. Extend the source-owned ServerSnapshotImage in khz_server_snapshot_checkpoint.inl (Network part1) with KHZ_ArchiveSidecarImage:

- Its boundary must also reject g_archivingSnapshot or an active archive attempt.
- Capture KHZ_ArchiveCopySidecar alongside header/message/temp-buffer capture; on failure return the bounded native rejection before setting captured=true.
- During ALL-provider validation, require KHZ_ArchiveValidateSidecar(image.archive, image.frame.nativeTime). Pass the saved frame's native time, not the current branch time.
- Restore the sidecar under the same actual paused owner/fence after ALL validations pass. Network part0 restores native bytes/cache storage; this sidecar restores only original-epoch scheduler/committed tags/availability/diagnostic counters. There are no callbacks, allocations or setters in its commit.
- Discard the provider's captured flag with its other image fields. The archive remains ordinary gameplay state; discarding the checkpoint does not reset it.

The sidecar additionally stores the captured typed descriptors for validation against saved native counters, contiguous extents, strictly increasing times, full/delta references, and a recomputed availability mask. This is metadata validation, not a second restore owner for native svs. Native Network part0's captured svs descriptor/counter image must be cross-checked against this same capture tuple before ANY commit. The draft supplies KHZ_ArchiveSidecarMatchesNativeImage(image, actualCapturedSvs). Add a typed source-owned Network helper that validates networkImage.captured/statics and calls this API with its actual saved svs allocation; Network part1 must require that helper during ALL validation. Comparing only against the current branch is insufficient.

All existing required Network parts and all 14 domain bits remain mandatory. Same map/assets/arena pointers, actual private host pause, channel drain, producer/Physics/Collision fences, admission identity and pre-effect guards remain required. No pointer serialization, cross-process restore, counters-only restoration, ordinary hash replay attestation, or unsupported dynamic-physics claim is introduced.

## Contract/evidence gates before applying

The executed pure tests cover 20 recordings across 1,000 synthetic ticks; absolute phase across stalls; persistent logical delay under every 1 ms remainder; internal holes; oldest and byte clamping; circular id/bitmask wrap; independent slow-reference lookup over arbitrary holes; exact 1-second full-base limits; rejected foreign/self/future/delta/phantom identity; bitmap/counter mismatch; 64-row/descriptor headroom; range overflow; reset epoch; and a synthetic same-process policy branch restore. They do not restore a native world.

The final isolated run used `python staged/archive/run_contracts.py`. Exact argv, compiler configuration, per-stage exit status, logs, stable source hashes and executable hashes are in `results/20261003-122302-671923/result.json` beneath this directory. The policy binary SHA-256 is `794105df043f278ebe641a7dd8eb6579f11fe1335ed8e868f9d85650ba411f75`; the synthetic adapter binary SHA-256 is `5b317726be0d4629b443825f4274e6130a24c59f9aacef0cc2b504bf0f7d2ca3`. Assertions were enabled in both Release test targets. Earlier failed fixture link/type attempts remain in their original result directories; the final run did not change the staged adapter.

The actual native contract fixture must invoke the compiled native encoder/decoder and real same-process storage with a loaded pinned map. It needs:

1. Full/delta byte roundtrip for human+15 bots, actual cachedFrame.time, client PS/HUD fields, linked entity trajectory/visibility state and archive lookup at all 50 submillisecond-step remainders.
2. Force an actual full message overflow with real MSG writes. Preserve advanced cache counters/evictions, omit the archive descriptor, then reuse the id successfully. Reject the phantom full base and cached hit; verify retained payload values instead of inspecting counters alone.
3. Force a cached full base into the next descriptor slot and the client's eviction-edge rows. Decode the delta; verify a fresh full decode preserves complete native PS/client state and has no borrowed-slot overwrite.
4. Actual 32 MiB split-copy/split-read boundary, descriptor wrap, byte/base eviction, cached presence independent eligibility, invalid kind/time/base id, and corrupted/self/future/dead dependency refusal before recursion.
5. Stock killcam/follow modes with a real persistent archiveTime: normal ceiling keeps the chosen delay, oldest clamp is explicit, unavailable/current fallback is correct, timestamps remain rebased from actual sample time, and death/respawn/slot-reuse semantics are observed.
6. Legacy timestamp interpolation on stationary/linear paths, exact sample and between samples, current anchor, a400 ms request using an older bracket, dead/follow states, missing client, and lifecycle reset. Custom KHZ_RewindClient must continue to retain/query 250 distinct 1 ms samples.
7. Map/fast/full restart and server-error cleanup while stopped writers are actually quiescent. New epoch must reject stale cached identities and first SS_GAME recording must be immediate. No reset is allowed across a pinned checkpoint lifetime.
8. Actual ALL14 same-process capture/branch/restore under private pause: native archive byte/cache/descriptor image and this sidecar agree, repeated restored lookup/encoding match, and refused validation makes no commits. Missing domains or absent fences must continue to reject.
9. Actual 60-second 1h15bot and four-human Killhouse/Crash measurements: all 1 kHz simulation/unique human command/snapshot/debt gates remain exact, record archive attempts/work/overflow/skipped opportunities separately, and compare measured p99. No performance improvement is established by this draft.
