// Source-owned seam: include at END of xanim/xanim.cpp, COD4X1KHZ && KISAK_MP.
// The real dedicated native arena is distinct from the client-owner arena.
// Images stay in this process at their original addresses; never serialize
// them or use an asset fingerprint as proof of mutable-world restoration.
#include "khz_server_animation_checkpoint.h"
#include "khz_server_checkpoint_native.h"
#include <game_mp/g_main_mp.h>
#include <game_mp/g_utils_mp.h>
#include <script/scr_animtree.h>
#include <universal/com_memory.h>
#include <Windows.h>
#include <cmath>
#include <cstddef>

extern scrMemTreePub_t scrMemTreePub;
extern scrAnimGlob_t scrAnimGlob;
extern XAnim_s* s_mantleAnims;

namespace khz_server_animation_detail {
using namespace khz::server_native_checkpoint;
constexpr unsigned NodeCount = 4096, TreeCount = 72;
constexpr unsigned DefinitionCapacity = MAX_XANIMTREE_NUM + 2, DefinitionEntryLimit = 4096;
constexpr unsigned PartsCapacity = 4096, LoadedPartsCapacity = 32768;
constexpr std::size_t FingerprintByteLimit = 128u * 1024 * 1024;
constexpr std::size_t SingleArrayLimit = 16u * 1024 * 1024;

struct Signature { std::uint64_t a, b; };
struct Fingerprint {
    Signature value;
    std::size_t bytes;
    void begin() { value = {14695981039346656037ull, 7809847782465536322ull}; bytes = 0; }
    bool append(const void* p, std::size_t count);
};
struct DefinitionPin { const XAnim_s* address; unsigned size; Signature signature; };
struct PartsPin { const XAnimParts* address; Signature signature; };
struct Scalars { int usage, highUsage, notifyCount; unsigned endTag; bool developer, disableLeakCheck; };
struct Image {
    Bytes<sizeof(nativeXAnimInfo)> nodes;
    Bytes<sizeof(XAnimNotify_s) * 128> notifications;
    Bytes<sizeof(scrAnimPub)> scriptPublic;
    Bytes<sizeof(scrAnimGlob)> scriptGlobal;
    XAnimTree_s trees[TreeCount];
    Scalars scalars;
    KHZ_ServerCheckpointFrame frame;
    DefinitionPin definitions[DefinitionCapacity];
    PartsPin parts[PartsCapacity];
    unsigned definitionCount, partsCount;
    bool captured;
};
// POD BSS: no large aggregate temporary in the CRT initializer.
Image image;
XAnimTree_s* boundTrees[TreeCount];
const XAnim_s* boundGeneric;
const void* boundHunk;
bool bound, registered;
XAnimParts* loadedParts[LoadedPartsCapacity];
unsigned loadedPartsCount;
bool loadedPartsOverflow;
std::size_t fingerprintWorkBytes;

bool same(const Signature& a, const Signature& b) { return a.a == b.a && a.b == b.b; }
bool readable(const void* pointer, std::size_t bytes) {
    auto at = reinterpret_cast<std::uintptr_t>(pointer);
    if (!at || !bytes || bytes > UINTPTR_MAX - at) return false;
    const auto end = at + bytes;
    while (at < end) {
        MEMORY_BASIC_INFORMATION memory;
        if (!VirtualQuery(reinterpret_cast<const void*>(at), &memory, sizeof(memory)) ||
            memory.State != MEM_COMMIT || (memory.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
        const unsigned protect = memory.Protect & 0xff;
        if (protect != PAGE_READONLY && protect != PAGE_READWRITE && protect != PAGE_WRITECOPY &&
            protect != PAGE_EXECUTE_READ && protect != PAGE_EXECUTE_READWRITE && protect != PAGE_EXECUTE_WRITECOPY)
            return false;
        const auto start = reinterpret_cast<std::uintptr_t>(memory.BaseAddress);
        if (memory.RegionSize > UINTPTR_MAX - start) return false;
        const auto next = start + memory.RegionSize;
        if (next <= at) return false;
        at = next < end ? next : end;
    }
    return true;
}
bool Fingerprint::append(const void* pointer, std::size_t count) {
    if (!count) return true;
    if (count > SingleArrayLimit || bytes > FingerprintByteLimit - count ||
        fingerprintWorkBytes > FingerprintByteLimit - count || !readable(pointer, count)) return false;
    const auto* data = static_cast<const unsigned char*>(pointer);
    for (std::size_t i = 0; i < count; ++i) {
        value.a = (value.a ^ data[i]) * 1099511628211ull;
        value.b = (value.b ^ (unsigned(data[i]) + 1)) * 14029467366897019727ull;
    }
    bytes += count; fingerprintWorkBytes += count; return true;
}
bool stringFingerprint(Fingerprint& hash, const char* value) {
    if (!value) return true; // Optional native debug names may be absent.
    for (unsigned n = 0; n < 4096; ++n) {
        if (!hash.append(value + n, 1)) return false;
        if (!value[n]) return true;
    }
    return false;
}
bool arrayFingerprint(Fingerprint& hash, const void* value, std::size_t count, std::size_t stride) {
    if (count > SingleArrayLimit / stride) return false;
    return !count || (value && hash.append(value, count * stride));
}
bool nativeString(unsigned id, unsigned knownSize = 0, bool terminated = true) {
    if (!id || id >= MEMORY_NODE_COUNT || scrMemTreePub.mt_buffer != reinterpret_cast<char*>(scrMemTreeGlob.nodes))
        return false;
    const auto* ref = reinterpret_cast<const RefString*>(scrMemTreePub.mt_buffer + id * MT_NODE_SIZE);
    if (!KHZ_ServerCheckpointRestoresRange(KHZ_ServerCheckpointDomain::ScriptAllocators, ref, sizeof(std::uint32_t)) ||
        !ref->refCount) return false;
    unsigned size = knownSize;
    if (!size) {
        for (size = 1; size <= 4096; ++size) {
            if (!KHZ_ServerCheckpointRestoresRange(KHZ_ServerCheckpointDomain::ScriptAllocators, ref,
                sizeof(std::uint32_t) + size)) return false;
            if (!ref->str[size - 1]) break;
        }
        if (size > 4096) return false;
    }
    return KHZ_ServerCheckpointRestoresRange(KHZ_ServerCheckpointDomain::ScriptAllocators, ref,
        sizeof(std::uint32_t) + size) && ref->byteLen == (size & 0xffu) &&
        (!terminated || !ref->str[size - 1]);
}
bool textReference(Fingerprint& hash, unsigned id) {
    if (!nativeString(id)) return false;
    const auto* ref = reinterpret_cast<const RefString*>(scrMemTreePub.mt_buffer + id * MT_NODE_SIZE);
    // IDs and text are immutable asset dependencies; live refcounts are mutable
    // MT/SL storage, deliberately excluded from the immutable fingerprint.
    return hash.append(&id, sizeof(id)) && stringFingerprint(hash, ref->str);
}
void __cdecl enumerateParts(XAssetHeader header, void*) {
    if (!header.parts) return;
    if (loadedPartsCount == LoadedPartsCapacity) { loadedPartsOverflow = true; return; }
    loadedParts[loadedPartsCount++] = header.parts;
}
bool loadAssetInventory(char* text, unsigned capacity) {
    if (!IsFastFileLoad()) return reject(text, capacity, "server_animation_checkpoint_requires_pinned_fastfile_assets");
    loadedPartsCount = 0; loadedPartsOverflow = false; fingerprintWorkBytes = 0;
    DB_EnumXAssets(ASSET_TYPE_XANIMPARTS, enumerateParts, nullptr, true);
    return !loadedPartsOverflow ? true : reject(text, capacity, "server_animation_loaded_parts_inventory_capacity_exceeded");
}
bool loadedAsset(const XAnimParts* parts) {
    for (unsigned i = 0; i < loadedPartsCount; ++i) if (loadedParts[i] == parts) return true;
    return false;
}
bool deltaFingerprint(Fingerprint& hash, const XAnimParts& parts) {
    if (!parts.deltaPart) return true;
    if (!hash.append(parts.deltaPart, sizeof(XAnimDeltaPart))) return false;
    const auto* trans = parts.deltaPart->trans;
    if (trans) {
        if (!hash.append(trans, offsetof(XAnimPartTrans, u))) return false;
        if (!trans->size) {
            if (!hash.append(&trans->u.frame0, sizeof(float) * 3)) return false;
        } else {
            // db_load.cpp stores a 28-byte frame prefix, inline size+1 indices,
            // and a separately allocated 3-component frame array. sizeof the
            // declared C struct would overread the compact size==0 variant.
            if (trans->size > parts.numframes || trans->smallTrans > 1 ||
                !hash.append(&trans->u.frames, offsetof(XAnimPartTransFrames, indices)) ||
                !arrayFingerprint(hash, &trans->u.frames.indices, unsigned(trans->size) + 1, parts.numframes >= 256 ? 2 : 1) ||
                !arrayFingerprint(hash, trans->u.frames.frames._1, unsigned(trans->size) + 1, trans->smallTrans ? 3 : 6)) return false;
        }
    }
    const auto* quat = parts.deltaPart->quat;
    if (quat) {
        if (!hash.append(quat, offsetof(XAnimDeltaPartQuat, u))) return false;
        if (!quat->size) {
            if (!hash.append(&quat->u.frame0, sizeof(short) * 2)) return false;
        } else if (quat->size > parts.numframes ||
            !hash.append(&quat->u.frames, offsetof(XAnimDeltaPartQuatDataFrames, indices)) ||
            !arrayFingerprint(hash, &quat->u.frames.indices, unsigned(quat->size) + 1, parts.numframes >= 256 ? 2 : 1) ||
            !arrayFingerprint(hash, quat->u.frames.frames, unsigned(quat->size) + 1, sizeof(short) * 2)) return false;
    }
    return true;
}
bool partsFingerprint(const XAnimParts* parts, Signature& output) {
    if (!parts || !loadedAsset(parts)) return false;
    Fingerprint hash; hash.begin();
    if (!hash.append(parts, sizeof(*parts)) || !parts->name || !stringFingerprint(hash, parts->name) ||
        !std::isfinite(parts->framerate) || !std::isfinite(parts->frequency) ||
        !arrayFingerprint(hash, parts->names, parts->boneCount[9], sizeof(unsigned short)) ||
        !arrayFingerprint(hash, parts->notify, parts->notifyCount, sizeof(XAnimNotifyInfo)) ||
        !arrayFingerprint(hash, parts->dataByte, parts->dataByteCount, 1) ||
        !arrayFingerprint(hash, parts->dataShort, parts->dataShortCount, 2) ||
        !arrayFingerprint(hash, parts->dataInt, parts->dataIntCount, 4) ||
        !arrayFingerprint(hash, parts->randomDataByte, parts->randomDataByteCount, 1) ||
        !arrayFingerprint(hash, parts->randomDataShort, parts->randomDataShortCount, 2) ||
        !arrayFingerprint(hash, parts->randomDataInt, parts->randomDataIntCount, 4) ||
        !arrayFingerprint(hash, parts->indices.data, parts->indexCount, parts->numframes >= 256 ? 2 : 1) ||
        !deltaFingerprint(hash, *parts)) return false;
    for (unsigned i = 0; i < parts->boneCount[9]; ++i) if (!textReference(hash, parts->names[i])) return false;
    float previous = -1;
    for (unsigned i = 0; i < parts->notifyCount; ++i) {
        const auto& note = parts->notify[i];
        if (!std::isfinite(note.time) || note.time < previous || note.time < 0 || note.time > 1 ||
            !textReference(hash, note.name)) return false;
        previous = note.time;
    }
    output = hash.value; return true;
}
bool definitionFingerprint(const XAnim_s* definition, const KHZ_ServerCheckpointFrame& f, Signature& output) {
    if (!definition || !inside(definition, reinterpret_cast<const void*>(f.hunkBaseAddress), f.hunkLowUsed,
        offsetof(XAnim_s, entries)) || !definition->size || definition->size > DefinitionEntryLimit) return false;
    const auto bytes = offsetof(XAnim_s, entries) + sizeof(XAnimEntry) * definition->size;
    if (!inside(definition, reinterpret_cast<const void*>(f.hunkBaseAddress), f.hunkLowUsed, bytes)) return false;
    Fingerprint hash; hash.begin();
    if (!hash.append(definition, bytes) || !stringFingerprint(hash, definition->debugName)) return false;
    if (definition->debugAnimNames) {
        if (!arrayFingerprint(hash, definition->debugAnimNames, definition->size, sizeof(const char*))) return false;
        for (unsigned i = 0; i < definition->size; ++i)
            if (!stringFingerprint(hash, definition->debugAnimNames[i])) return false;
    }
    if (definition->entries[0].parent) return false;
    for (unsigned i = 0; i < definition->size; ++i) {
        const auto& entry = definition->entries[i];
        if (entry.parent >= definition->size) return false;
        if (entry.numAnims) {
            if (entry.animParent.children >= definition->size || entry.numAnims > definition->size - entry.animParent.children)
                return false;
            for (unsigned c = 0; c < entry.numAnims; ++c)
                if (definition->entries[entry.animParent.children + c].parent != i) return false;
        } else if (!entry.parts || !loadedAsset(entry.parts)) return false;
        unsigned parent = i;
        for (unsigned depth = 0; parent; ++depth) {
            if (depth == definition->size || definition->entries[parent].parent == parent) return false;
            parent = definition->entries[parent].parent;
        }
    }
    output = hash.value; return true;
}
bool pinParts(const XAnimParts* parts, char* text, unsigned capacity) {
    for (unsigned i = 0; i < image.partsCount; ++i) if (image.parts[i].address == parts) return true;
    if (image.partsCount == PartsCapacity) return reject(text, capacity, "server_animation_parts_pin_capacity_exceeded");
    Signature signature;
    if (!partsFingerprint(parts, signature)) return reject(text, capacity, "server_animation_native_parts_dependency_is_unsupported");
    image.parts[image.partsCount++] = {parts, signature}; return true;
}
bool pinDefinition(const XAnim_s* definition, const KHZ_ServerCheckpointFrame& f, char* text, unsigned capacity) {
    for (unsigned i = 0; i < image.definitionCount; ++i) if (image.definitions[i].address == definition) return true;
    if (image.definitionCount == DefinitionCapacity) return reject(text, capacity, "server_animation_definition_pin_capacity_exceeded");
    Signature signature;
    if (!definitionFingerprint(definition, f, signature)) return reject(text, capacity, "server_animation_definition_outside_original_low_hunk_or_invalid");
    image.definitions[image.definitionCount++] = {definition, definition->size, signature};
    for (unsigned i = 0; i < definition->size; ++i)
        if (!definition->entries[i].numAnims && !pinParts(definition->entries[i].parts, text, capacity)) return false;
    return true;
}
XAnimTree_s* nativeTree(unsigned i) {
    return i < 64 ? level_bgs.clientinfo[i].pXAnimTree : g_scr_data.playerCorpseInfo[i - 64].tree;
}
bool graphState(const KHZ_ServerCheckpointFrame& f, char* text, unsigned capacity) {
    if (!context(text, capacity) || !KHZ_ServerScriptCheckpointIdle(text, capacity)) return false;
    if (!bound || ownerAnimationDomain() || boundHunk != reinterpret_cast<const void*>(f.hunkBaseAddress) ||
        boundGeneric != level_bgs.generic_human.tree.anims || level.time != f.nativeTime || bgs ||
        scrAnimPub.animtree_loading || scrAnimPub.animtree_node || scrAnimPub.xanim_num[0] ||
        scrAnimPub.xanim_num[1] >= MAX_XANIMTREE_NUM || g_info_usage < 1 || g_info_usage > int(NodeCount) ||
        g_info_high_usage < g_info_usage || g_info_high_usage > int(NodeCount) ||
        g_notifyListSize < 0 || g_notifyListSize > 128 || !nativeString(g_endNotetrackName))
        return reject(text, capacity, "server_animation_not_original_idle_dedicated_arena");
    const auto* definition = boundGeneric;
    if (!definition || !inside(definition, reinterpret_cast<const void*>(f.hunkBaseAddress), f.hunkLowUsed,
        offsetof(XAnim_s, entries)) || !definition->size || definition->size > DefinitionEntryLimit ||
        !inside(definition, reinterpret_cast<const void*>(f.hunkBaseAddress), f.hunkLowUsed,
            offsetof(XAnim_s, entries) + sizeof(XAnimEntry) * definition->size))
        return reject(text, capacity, "server_animation_generic_definition_not_original_low_hunk");
    unsigned char seen[NodeCount]{};
    unsigned treeUsage[TreeCount]{};
    unsigned freeCount = 0, previous = 0, index = nativeXAnimInfo[0].next;
    if (nativeXAnimInfo[0].inuse || nativeXAnimInfo[0].prev >= NodeCount)
        return reject(text, capacity, "server_animation_free_list_sentinel_invalid");
    while (index) {
        if (index >= NodeCount || seen[index] || nativeXAnimInfo[index].inuse || nativeXAnimInfo[index].prev != previous)
            return reject(text, capacity, "server_animation_free_list_cycle_or_nonreciprocal_link");
        seen[index] = 1; ++freeCount; previous = index; index = nativeXAnimInfo[index].next;
    }
    if (nativeXAnimInfo[0].prev != previous)
        return reject(text, capacity, "server_animation_free_list_tail_is_not_sentinel_previous");
    for (unsigned t = 0; t < TreeCount; ++t) {
        const auto* tree = nativeTree(t);
        if (tree != boundTrees[t] || !inside(tree, reinterpret_cast<const void*>(f.hunkBaseAddress), f.hunkLowUsed, sizeof(*tree)) ||
            !KHZ_ServerCheckpointRestoresRange(KHZ_ServerCheckpointDomain::Animation, tree, sizeof(*tree)) ||
            tree->anims != definition || tree->calcRefCount || tree->modifyRefCount ||
            tree->children >= NodeCount || tree->info_usage < 0 || tree->info_usage >= int(NodeCount))
            return reject(text, capacity, "server_animation_native_tree_identity_refcount_or_restore_span_invalid");
        // Every active node must be connected to exactly one recorded tree.
        // Parent chains are bounded by the arena; sibling reciprocity is checked
        // separately below, so no unbounded recursive native walk is needed.
        for (unsigned n = 1; n < NodeCount; ++n) {
            const auto& node = nativeXAnimInfo[n];
            if (!node.inuse || node.tree != tree) continue;
            if (seen[n] || node.prev >= NodeCount || node.next >= NodeCount || node.parent >= NodeCount ||
                node.children >= NodeCount || node.animIndex >= definition->size || node.notifyChild >= definition->size)
                return reject(text, capacity, "server_animation_active_node_link_or_definition_index_invalid");
            seen[n] = 2; ++treeUsage[t];
            const auto& entry = definition->entries[node.animIndex];
            if (node.parts != entry.parts || bool(node.animToModel) != !entry.numAnims ||
                (node.animToModel && (node.children || !loadedAsset(node.parts) || !readable(node.parts, sizeof(XAnimParts)))) ||
                (node.notifyName && !nativeString(node.notifyName)))
                return reject(text, capacity, "server_animation_native_node_definition_or_string_map_invalid");
            if (node.animToModel) {
                if (scrMemTreePub.mt_buffer != reinterpret_cast<char*>(scrMemTreeGlob.nodes) ||
                    !KHZ_ServerCheckpointRestoresRange(KHZ_ServerCheckpointDomain::ScriptAllocators,
                        scrMemTreePub.mt_buffer + node.animToModel * MT_NODE_SIZE, sizeof(std::uint32_t) + 17))
                    return reject(text, capacity, "server_animation_native_anim_to_model_header_has_no_mt_restore_span");
                const auto* ref = reinterpret_cast<const RefString*>(scrMemTreePub.mt_buffer + node.animToModel * MT_NODE_SIZE);
                const unsigned mapSize = 17u + static_cast<unsigned char>(ref->str[16]);
                if (!nativeString(node.animToModel, mapSize, false) || static_cast<unsigned char>(ref->str[16]) != node.parts->boneCount[9])
                    return reject(text, capacity, "server_animation_native_anim_to_model_payload_length_invalid");
            }
            const XAnimParts* notifyParts = node.animToModel ? node.parts :
                (node.notifyChild ? definition->entries[node.notifyChild].parts : nullptr);
            if (node.notifyChild && definition->entries[node.notifyChild].numAnims)
                return reject(text, capacity, "server_animation_notetrack_child_is_not_native_leaf");
            if (node.notifyIndex < -1 || (node.notifyIndex >= 0 && (!notifyParts || !loadedAsset(notifyParts) || !readable(notifyParts, sizeof(XAnimParts)) ||
                node.notifyIndex >= notifyParts->notifyCount)))
                return reject(text, capacity, "server_animation_notetrack_cursor_outside_pinned_parts");
            const auto& state = node.state;
            if (!std::isfinite(state.currentAnimTime) || !std::isfinite(state.oldTime) || !std::isfinite(state.goalTime) ||
                !std::isfinite(state.goalWeight) || !std::isfinite(state.weight) || !std::isfinite(state.rate) ||
                state.currentAnimTime < 0 || state.currentAnimTime > 1 || state.oldTime < 0 || state.oldTime > 1 ||
                state.goalTime < 0 || state.goalWeight < 0 || state.weight < 0)
                return reject(text, capacity, "server_animation_native_state_is_nonfinite_or_outside_native_limits");
            if (node.prev) {
                const auto& sibling = nativeXAnimInfo[node.prev];
                if (!sibling.inuse || sibling.tree != tree || sibling.parent != node.parent || sibling.next != n || sibling.animIndex > node.animIndex)
                    return reject(text, capacity, "server_animation_previous_sibling_is_not_reciprocal");
            } else if ((node.parent ? nativeXAnimInfo[node.parent].children : tree->children) != n)
                return reject(text, capacity, "server_animation_first_child_is_not_owner_root");
            if (node.next) {
                const auto& sibling = nativeXAnimInfo[node.next];
                if (!sibling.inuse || sibling.tree != tree || sibling.parent != node.parent || sibling.prev != n || sibling.animIndex < node.animIndex)
                    return reject(text, capacity, "server_animation_next_sibling_is_not_reciprocal");
            }
            if (node.children) {
                const auto& child = nativeXAnimInfo[node.children];
                if (!child.inuse || child.tree != tree || child.parent != n || child.prev)
                    return reject(text, capacity, "server_animation_child_link_is_not_reciprocal");
            }
            unsigned firstSibling = n;
            for (unsigned depth = 0; nativeXAnimInfo[firstSibling].prev; ++depth) {
                const unsigned before = nativeXAnimInfo[firstSibling].prev;
                if (depth == NodeCount || before >= NodeCount || !nativeXAnimInfo[before].inuse ||
                    nativeXAnimInfo[before].tree != tree || nativeXAnimInfo[before].parent != node.parent ||
                    nativeXAnimInfo[before].next != firstSibling)
                    return reject(text, capacity, "server_animation_sibling_cycle_or_foreign_owner");
                firstSibling = before;
            }
            if ((node.parent ? nativeXAnimInfo[node.parent].children : tree->children) != firstSibling)
                return reject(text, capacity, "server_animation_sibling_list_is_unreachable_from_native_parent");
            unsigned ancestor = n;
            for (unsigned depth = 0; nativeXAnimInfo[ancestor].parent; ++depth) {
                const unsigned parent = nativeXAnimInfo[ancestor].parent;
                if (depth == NodeCount || parent >= NodeCount || !nativeXAnimInfo[parent].inuse ||
                    nativeXAnimInfo[parent].tree != tree || nativeXAnimInfo[parent].animIndex >= definition->size ||
                    definition->entries[nativeXAnimInfo[ancestor].animIndex].parent != nativeXAnimInfo[parent].animIndex)
                    return reject(text, capacity, "server_animation_native_parent_graph_cycle_or_asset_mismatch");
                ancestor = parent;
            }
            if (nativeXAnimInfo[ancestor].prev) {
                unsigned first = ancestor;
                for (unsigned depth = 0; nativeXAnimInfo[first].prev; ++depth) {
                    const unsigned before = nativeXAnimInfo[first].prev;
                    if (depth == NodeCount || before >= NodeCount || !nativeXAnimInfo[before].inuse ||
                        nativeXAnimInfo[before].tree != tree || nativeXAnimInfo[before].parent || nativeXAnimInfo[before].next != first)
                        return reject(text, capacity, "server_animation_root_sibling_cycle_or_invalid_link");
                    first = before;
                }
                ancestor = first;
            }
            if (tree->children != ancestor)
                return reject(text, capacity, "server_animation_active_node_is_unreachable_from_tree_root");
        }
        if (treeUsage[t] != unsigned(tree->info_usage))
            return reject(text, capacity, "server_animation_tree_usage_does_not_cover_actual_nodes");
    }
    unsigned used = 0;
    for (unsigned i = 1; i < NodeCount; ++i) {
        if (!seen[i] || bool(nativeXAnimInfo[i].inuse) != (seen[i] == 2))
            return reject(text, capacity, "server_animation_arena_has_foreign_tree_or_uncovered_allocation");
        used += seen[i] == 2;
    }
    if (used + 1 != unsigned(g_info_usage) || used + freeCount != NodeCount - 1)
        return reject(text, capacity, "server_animation_native_allocator_usage_coverage_incomplete");
    for (int i = 0; i < g_notifyListSize; ++i) {
        const auto& notification = g_notifyList[i];
        const auto p = reinterpret_cast<std::uintptr_t>(notification.name), base = reinterpret_cast<std::uintptr_t>(scrMemTreePub.mt_buffer);
        if (p < base + 4 || p - base < 4 || (p - base - 4) % MT_NODE_SIZE ||
            !nativeString(unsigned((p - base - 4) / MT_NODE_SIZE)) || !std::isfinite(notification.timeFrac) ||
            notification.timeFrac < 0 || notification.timeFrac > 1 ||
            (i && g_notifyList[i - 1].timeFrac > notification.timeFrac))
            return reject(text, capacity, "server_animation_notification_pointer_or_native_order_invalid");
    }
    return true;
}
bool capture(const KHZ_ServerCheckpointFrame& f, char* text, unsigned capacity) {
    if (!context(text, capacity) || !loadAssetInventory(text, capacity) || !graphState(f, text, capacity)) return false;
    image.definitionCount = image.partsCount = 0;
    if (!pinDefinition(boundGeneric, f, text, capacity) || !pinDefinition(s_mantleAnims, f, text, capacity)) return false;
    for (unsigned i = 1; i <= scrAnimPub.xanim_num[1]; ++i)
        if (!pinDefinition(scrAnimPub.xanim_lookup[1][i].anims, f, text, capacity)) return false;
    image.nodes.capture(nativeXAnimInfo); image.notifications.capture(g_notifyList);
    image.scriptPublic.capture(&scrAnimPub); image.scriptGlobal.capture(&scrAnimGlob);
    for (unsigned i = 0; i < TreeCount; ++i) std::memcpy(&image.trees[i], boundTrees[i], sizeof(XAnimTree_s));
    image.scalars = {g_info_usage, g_info_high_usage, g_notifyListSize, g_endNotetrackName, g_anim_developer, g_disableLeakCheck};
    image.frame = f; image.captured = true; return true;
}
bool validate(const KHZ_ServerCheckpointFrame& f, char* text, unsigned capacity) {
    if (!image.captured || !khz::server_checkpoint::sameLifetime(image.frame, f))
        return reject(text, capacity, "server_animation_image_map_hunk_or_fence_identity_changed");
    if (!context(text, capacity) || !loadAssetInventory(text, capacity) || !graphState(f, text, capacity)) return false;
    const auto* savedPublic = reinterpret_cast<const scrAnimPub_t*>(image.scriptPublic.value);
    const auto* savedGlobal = reinterpret_cast<const scrAnimGlob_t*>(image.scriptGlobal.value);
    if (std::memcmp(savedPublic->xanim_lookup, scrAnimPub.xanim_lookup, sizeof(scrAnimPub.xanim_lookup)) ||
        std::memcmp(savedPublic->xanim_num, scrAnimPub.xanim_num, sizeof(scrAnimPub.xanim_num)) ||
        savedGlobal->start != scrAnimGlob.start || savedGlobal->pos != scrAnimGlob.pos ||
        g_endNotetrackName != image.scalars.endTag)
        return reject(text, capacity, "server_animation_loaded_script_lookup_parser_or_end_tag_changed");
    for (unsigned i = 0; i < image.definitionCount; ++i) {
        const auto& pin = image.definitions[i]; Signature signature;
        if (!definitionFingerprint(pin.address, f, signature) || pin.address->size != pin.size || !same(pin.signature, signature))
            return reject(text, capacity, "server_animation_immutable_native_definition_identity_or_content_changed");
    }
    for (unsigned i = 0; i < image.partsCount; ++i) {
        const auto& pin = image.parts[i]; Signature signature;
        if (!partsFingerprint(pin.address, signature) || !same(pin.signature, signature))
            return reject(text, capacity, "server_animation_immutable_fastfile_parts_identity_or_content_changed");
    }
    return true;
}
void restore(const KHZ_ServerCheckpointFrame&) {
    iassert(KHZ_ServerCheckpointContext() && image.captured && !ownerAnimationDomain());
    // MT/SL commit already restored exact allocation bytes and reference counts.
    // Never call native Free/AddRef/RemoveRef/XAnim gameplay from this commit.
    image.nodes.restore(nativeXAnimInfo); image.notifications.restore(g_notifyList);
    image.scriptPublic.restore(&scrAnimPub); image.scriptGlobal.restore(&scrAnimGlob);
    for (unsigned i = 0; i < TreeCount; ++i) std::memcpy(boundTrees[i], &image.trees[i], sizeof(XAnimTree_s));
    g_info_usage = image.scalars.usage; g_info_high_usage = image.scalars.highUsage;
    g_notifyListSize = image.scalars.notifyCount; g_endNotetrackName = image.scalars.endTag;
    g_anim_developer = image.scalars.developer; g_disableLeakCheck = image.scalars.disableLeakCheck;
}
void discard() { image.captured = false; image.definitionCount = image.partsCount = 0; }
}

void KHZ_ServerAnimationRegisterCheckpoint() {
    using namespace khz_server_animation_detail;
    const KHZ_ServerCheckpointProvider provider{KHZ_ServerCheckpointDomain::Animation, 0, sizeof(image),
        capture, validate, restore, discard};
    if (registered || !KHZ_ServerCheckpointRegister(provider) ||
        !KHZ_ServerCheckpointRegisterRange(provider.domain, nativeXAnimInfo, sizeof(nativeXAnimInfo)) ||
        !KHZ_ServerCheckpointRegisterRange(provider.domain, g_notifyList, sizeof(XAnimNotify_s) * 128) ||
        !KHZ_ServerCheckpointRegisterRange(provider.domain, &scrAnimPub, sizeof(scrAnimPub)) ||
        !KHZ_ServerCheckpointRegisterRange(provider.domain, &scrAnimGlob, sizeof(scrAnimGlob)))
        Com_Error(ERR_FATAL, "Could not register actual dedicated native animation checkpoint");
    registered = true;
}
bool KHZ_ServerAnimationBindMapTrees(char* text, unsigned capacity) {
    using namespace khz_server_animation_detail;
    if (!registered || !Sys_IsMainThread() || !com_dedicated || !com_dedicated->current.integer ||
        KHZ_ServerCheckpointPinned() || ownerAnimationDomain() || !s_hunkData || s_hunkTotal <= 0 ||
        !level_bgs.generic_human.tree.anims || level_bgs.anim_user != 1)
        return reject(text, capacity, "server_animation_tree_bind_requires_loaded_unpinned_dedicated_owner");
    XAnimTree_s* trees[TreeCount];
    for (unsigned i = 0; i < TreeCount; ++i) {
        trees[i] = nativeTree(i);
        if (reinterpret_cast<std::uintptr_t>(trees[i]) % alignof(XAnimTree_s) ||
            !inside(trees[i], s_hunkData, unsigned(s_hunkTotal), sizeof(XAnimTree_s)) || !readable(trees[i], sizeof(XAnimTree_s)) ||
            trees[i]->anims != level_bgs.generic_human.tree.anims || trees[i]->calcRefCount || trees[i]->modifyRefCount)
            return reject(text, capacity, "server_animation_map_tree_is_foreign_busy_or_missing");
        for (unsigned j = 0; j < i; ++j) if (trees[j] == trees[i])
            return reject(text, capacity, "server_animation_map_tree_allocation_is_aliased");
    }
    // Replace exact map spans atomically without changing static arenas or
    // other providers. A failed replacement cannot advertise a partial map.
    bound = false;
    const void* bases[TreeCount]; std::size_t sizes[TreeCount];
    for (unsigned i = 0; i < TreeCount; ++i) { bases[i] = trees[i]; sizes[i] = sizeof(XAnimTree_s); }
    if (!KHZ_ServerCheckpointReplaceMapRanges(KHZ_ServerCheckpointDomain::Animation, bases, sizes, TreeCount))
        return reject(text, capacity, "server_animation_exact_tree_restore_range_registration_failed");
    std::memcpy(boundTrees, trees, sizeof(trees)); boundHunk = s_hunkData;
    boundGeneric = level_bgs.generic_human.tree.anims; bound = true; return true;
}
bool KHZ_ServerAnimationAllowAssetMutation(const char* operation) {
    if (!Sys_IsMainThread() || !com_dedicated || !com_dedicated->current.integer || !KHZ_ServerCheckpointPinned()) return true;
    KHZ_ServerCheckpointRejectExternalEffect(operation ? operation : "server_animation_pinned_asset_or_tree_lifetime_mutation");
    return false;
}
bool KHZ_ServerAnimationAllowTreeMutation(const XAnimTree_s* tree, const char* operation) {
    using namespace khz_server_animation_detail;
    if (!Sys_IsMainThread() || !com_dedicated || !com_dedicated->current.integer || !KHZ_ServerCheckpointPinned()) return true;
    if (bound && tree) for (unsigned i = 0; i < TreeCount; ++i)
        if (tree == boundTrees[i] && tree->anims == boundGeneric) return true;
    KHZ_ServerCheckpointRejectExternalEffect(operation ? operation : "server_animation_foreign_tree_allocator_mutation");
    return false;
}
