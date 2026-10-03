# Private plugin filesystem adapter

The private filesystem adapter is wired in Build57. Its isolated native backend passed 69 assertions; the actual rebuilt SDK DLL passed 28 filesystem assertions, including stored/deflated/classic-encrypted IWD reads, owner completion/free and final unload. Exact hashes and limits are recorded in `docs/evidence/plugin-native-build57-io.json`. Active-work cancellation/unload, concurrent filesystem barriers and native human pure-handshake coverage remain release gates; these scoped checks do not establish full public parity.

`engine/src/khz/khz_plugin_fs.cpp` owns private Win32 handles and a symbol-prefixed copy of the pinned CoD4x minizip decoder. It never reads native `fsh`, mutable package handles, search-path pointers or dvars from a worker. The decoder translation unit includes the upstream source with its original notices, prefixes its global symbols and uses the foundation zlib ABI. Its unused unconfined convenience opener is disabled. No upstream checkout is modified.

The native control owner copies the ordered search descriptors, home/base/game roots, language eligibility and pure/loose flags in `khz_plugin_fs_adapter.cpp`. Catalog publication assigns a new epoch. An old stream token cannot read after publication, even if a new stream reuses its slot. Paths normalize backslashes and reject traversal, device names, drive/ADS syntax and reparse ancestors. Each opened file's final handle path is checked before any truncation. Project home/base roots must remain beneath the captured project root.

The adapted SDK retains the public file names and signatures, with explicit bounded resource policies:

| Resource | Limit / ownership |
| --- | --- |
| Modules / private streams | 16 modules, 16 streams each |
| Copied search records | 512 in native search order |
| Retained full-file buffers | 8 per module; 16 MiB file maximum; 64 MiB global including NUL terminators |
| Streaming call | At most 1 MiB; actual bytes read/written |
| Stream / archive extent | At most 2 GiB minus one; pinned decoder remains a 32-bit ZIP implementation |
| Copied reference journal | 128 records, pending duplicates coalesced; reject a pure-sensitive read if its reference cannot be queued |
| Password | At most 63 bytes; classic PKZIP encryption |

Blocking opens, reads, writes, closes, copies and decoder/password calls require a pinned plugin job or thread. The authoritative owner rejects them before file access. `FreeFile` remains an owner operation outside ticks, including `OnUnload`. Normal writes target home/game; SV writes and the pinned `SV_WriteFile` alias target home; explicit base writes target base. SV reads prefer home then base. Normal reads use the copied native search order, including case-insensitive archive lookup and the native last-duplicate rule. `ReadLine` retains fgets-style 1/0/-1 behavior for SV loose reads only.

`Cancel` tombstones a plugin generation and schedules stream retirement. It preserves published full-file bytes until their owning generation frees them. `Discard` reclaims retained bytes only after `OnUnload` and all DLL/job/thread pins finish. `Shutdown` has the same caller barrier. Host storage remains allocated during process cleanup if a pinned caller persists, avoiding destruction of a live decoder, mutex or retirement thread.

The public `Plugin_unzSetPassword` uses an opaque cookie obtained by the added `Plugin_FS_GetZipHandle` helper. The cookie identifies the owner's exact private stream token; it is not a native engine or decoder pointer. Foreign, closed and reused cookies are rejected without dereferencing them. WinZip AES, Zip64, multi-volume archives and old binary SDK layouts are outside the pinned format/ABI contract.

Integration must initialize and capture before jobs start; invalidate at native FS shutdown; recapture after initial startup, restart restrictions/language changes and pure-list updates. Service copied references before plugin job completions outside gameplay ticks. Archive references revalidate epoch, package path and checksum against the current native list. Loose reads use a dedicated nonzero pure sentinel sequence so file access does not consume gameplay random state.

`scripts/plugin_fs_probe.py` prepares owned real stored/deflated/encrypted/CRC-failing archives, loose files and a reparse junction, then invokes `engine/tests/khz_plugin_fs_tests.cpp`. Python's independent ZIP decoder checks the generated encrypted bytes and bad CRC before native tests. The native fixture covers order, pure/language flags, paths, binary/empty files, line reads, actual append/copy, bounded storage, epoch/token reuse and retained-buffer cancellation. The actual DLL fixture in `sdk/tests/native_file_probe.inc` separately exercises the exported APIs from a pinned job and owner completion. Standalone success does not prove that native capture or DLL lifecycle integration works.
