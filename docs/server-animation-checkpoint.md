The staged native animation provider restores the dedicated server's actual
4,096 `XAnimInfo` records, circular free-list sentinel, allocation/high-water
counters, complete 72 player/corpse `XAnimTree_s` records, notification buffer,
developer/leak metadata, and exported `scrAnimPub`/`scrAnimGlob` storage. It
uses raw BSS images and commits with bounded copies and scalar assignments.
MT and SL commit first and restore the actual animation-to-model/string
payloads and reference counts. Animation commit invokes no refcount,
allocator, animation, script, file or notification callbacks.

`G_LoadAnimTreeInstances` creates 64 player and eight corpse trees with
`Hunk_AllocXAnimServer`, which calls `Hunk_AllocLow` with 32-byte alignment.
The provider advertises each exact 20-byte tree allocation separately;
alignment padding and the rest of the hunk are not advertised as restored.
No MP small-tree allocator was found: the `Com_XAnimCreateSmallTree` / MT
helper in `common.cpp` is SP-only. An active native node referring to any
other tree is explicitly ineligible. New/custom tree allocation requires a
real tree-allocation provider, not another address-range exception.

Integration is staged, not yet installed:

1. Include `khz_server_animation_checkpoint.inl` at the end of `xanim.cpp`
   under `COD4X1KHZ && KISAK_MP`. Call
   `KHZ_ServerAnimationRegisterCheckpoint()` at provider setup.
2. After the real `G_LoadAnimTreeInstances`, call
   `KHZ_ServerAnimationBindMapTrees(reason, capacity)` on the unpinned
   dedicated main owner. It registers all 72 exact live spans before World
   and DObj capture inspect their pointers. Capture rechecks the observer's
   original low-hunk extent, map/session/fence and actual loaded bindings.
   Registration capacity must accommodate these 72 entries. Before a new
   map is bound, the provider atomically replaces its 72 exact map ranges via
   `KHZ_ServerCheckpointReplaceMapRanges`, preserving static arenas and other
   providers. `KHZ_ServerCheckpointResetMapRanges` retires all dynamic ranges
   before a map is unloaded and refuses while an image is pinned. Neither API
   resets the coordinator or reduces any domain/part requirement. The native
   lifecycle call sites remain an integration requirement.
3. Before tree allocation/free, definition creation/blending/sync-node setup,
   asset cloning/precache/free, debug-definition release, or XAnim
   initialization/shutdown, check
   `KHZ_ServerAnimationAllowAssetMutation(operation)`. Refusal returns before
   native allocation/free/refcount changes and latches the branch failure.
   Relevant `xanim.cpp` entry points are `XAnimCreateTree`, `XAnimFreeTree`,
   `XAnimCreateAnims`, `XAnimCreate`, `XAnimBlend`, `XAnimSetupSyncNodes`,
   `XAnimFillInSyncNodes_r`, `XAnimClone`, `XAnimPrecache`, `XAnimFree`,
   `XAnimFreeList`, `XAnimFreeAnims`, `XAnimInit` and `XAnimShutdown`.
   Direct asset-loading entry points and script animation parsing require
   the same pre-side-effect guard, or an existing stronger pinned-asset
   lifecycle guard. Capture rejects an active animation-tree load/parser.
4. Native node allocate/free/clear/clone entry points use
   `KHZ_ServerAnimationAllowTreeMutation(tree, operation)` before their first
   counter/refcount write. The 72 recorded trees are supported during a
   branch; a foreign tree is refused. `XAnimClearTree` is restorable gameplay
   state and is not categorically blocked. Native mutation entry points
   include `XAnimAllocInfoWithParent`, `XAnimFreeInfo`, `XAnimCloneAnimTree`,
   and `XAnimClearTree`; setter paths using these must not change a foreign
   tree before reaching the guarded native allocation.

Capture/validation checks the full free/active allocation partition,
reciprocal sorted siblings, parent/child reachability, tree usage, native
definition indices/unions, real SL mapping lengths, notetrack cursors,
finite native phases and zero tree calculation/modification references.
Generic-human, mantle, and actual server script lookup definitions remain
at their original low-hunk addresses. Their entry/debug-name identity and
contents are pinned. Every referenced leaf must be an actual enumerated
loaded fastfile `XAnimParts`; loaded-object/custom external assets are
explicitly unsupported in the first fixture.

Immutable fingerprints cover the actual `XAnimParts` header, bone/notify
names and text, all native byte/short/int/random/index arrays, and delta
headers, inline indices and frame arrays. Compact zero-frame delta records
are read using the actual `db_load.cpp` layout, not a `sizeof` overread.
Bounds are 128 server definition slots plus generic/mantle, 4,096 unique
parts, 32,768 loaded assets, 4,096 entries per definition, 16 MiB per array
and 128 MiB total fingerprint input per operation. Identity/fingerprint
validation rejects changes; these immutable fingerprints do not restore
assets and do not attest full-world replay.

All mandatory server domains and provider parts still gate capture and all
providers validate before any restore write. The staged code has not been
built or run by this agent. A source compile and an actual paused native
capture/restore/repeated-branch fixture are separate pending validations.
