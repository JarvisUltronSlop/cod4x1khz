# Custom-pair content protocol

The protocol 10002 pair requests native content metadata before transferring missing mod/map files. The response is unsigned metadata from the accepted private server connection. It does not authenticate an official CoD4x service or replace a signed update manifest. The client selects an explicit `khz_download_origin`; a server response cannot change that origin.

Shared wire parsing is in [khz_content_protocol.h](../engine/src/khz/khz_content_protocol.h) and [khz_content_protocol.cpp](../engine/src/khz/khz_content_protocol.cpp). Version 1 commands contain exactly the following space-separated tokens, with canonical unsigned decimal scalars and a 64-character lowercase SHA256 digest:

```
khz_content 1 ID SESSION GENERATION QPATH
khz_content_meta 1 ID SESSION GENERATION BYTES SHA256 CHECKSUM PURECHECKSUM CHECKSUMFEED CHECKPURE QPATH URL
khz_content_error 1 ID SESSION GENERATION CODE
```

`GENERATION` is native `serverId`, distinct from the server's session and its internal monotonic client-admission serial. Commands cannot exceed 1023 bytes, the native reliable-command payload limit excluding its NUL. Paths are constrained to the shared safe-asset grammar; quoting, whitespace controls, traversal and alternate Windows device paths are rejected. IWD responses require native checksum and feed-dependent pure-checksum verification. Fastfiles carry SHA256/size with `CHECKPURE=0` and both checksum fields zero.

[khz_content_server.cpp](../engine/src/khz/khz_content_server.cpp) admits requests from proven human slots at `CS_CONNECTED` or later, including the native loading phase. Validation enqueues one fixed-size pending request per slot. Main service, outside `com_inServerFrame`, resolves at most two descriptors per pass from current native FS/DB advertisements. It permits the active mod or the current usermap's IWD/fastfiles and rejects stock/main asset requests. `sv_wwwBaseURL` currently supplies the explicit server HTTP origin; it must be a scheme/host/optional-port origin without a path prefix. The client independently validates the same origin.

The worker receives only copied admission data, paths, checksums and deadlines. It does not read native search paths, dvars, clients or gameplay state. It rechecks confinement and the opened handle's final path, holds a read-sharing-only handle across the SHA256/size and IWD checks, and bounds files to 1 GiB. Sixteen work descriptors and sixteen completion slots use backpressure. SHA reads check cancellation between 64 KiB chunks; map/session changes and error cleanup cancel the previous epoch. Native IWD directory verification is separately bounded by the shared verifier. Shutdown joins the worker before filesystem teardown.

Before emitting a reliable response, the owner rechecks full local key identity, monotonic slot-admission serial, current session, native serverId and connection state. A map change, disconnect or reused slot therefore invalidates outstanding work even when the same identity reconnects. Request IDs are monotonic per admission, duplicate requests do not hash again, and each slot admits at most one new request per 100 ms. Existing reliable-command retransmission delivers replies. Error replies are also rate bounded.

[content_server_policy_tests.cpp](../tests/content_server_policy_tests.cpp) exercises 16-slot bounds, duplicates, in-flight admission, rates, active-map/mod scope and stale completion rejection across identity, slot, session and map transitions. These fixtures do not establish native download parity. Release evidence still requires actual missing-asset metadata, HTTP transfer, SHA/pure validation, parked filesystem commit, restart/pure handshake and reconnect, plus tampering, cancellation and stale-admission cases. Server and missing-asset client fixtures need separate asset roots beneath the project.

Current source uses shared [khz_asset_paths.cpp](../engine/src/khz/khz_asset_paths.cpp) selection for native DB custom fastfile loading/advertisement and server metadata. It resolves only the active mod/current map, selects the project home root before base, and verifies that the copied worker path exactly matches the selected loaded fastfile. Native FS startup also mounts the active confined usermap's IWD. An unrelated file sharing a stock zone name cannot authorize a download. These new lifecycle hooks and actual usermap transfer remain unverified until native fixtures establish loading, advertisement, download and reconnect.

This Kisak foundation's native `Com_BlockChecksumKey32` uses seeded CRC32 polynomial `0xEDB88320`; native `FS_LoadZipFile` builds its directory CRC table from nonempty entries. The shared verifier must match that native policy rather than another engine's checksum algorithm. Release evidence requires comparison against actual FS-loaded IWD checksums and the native pure handshake, in addition to verifier self-consistency fixtures.
