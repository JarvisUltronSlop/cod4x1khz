// Source-owned fragment: END of qcommon/cm_load.cpp, opt-in custom MP/dedicated.
#include "khz_server_collision_checkpoint_util.h"
#include <qcommon/threads.h>
#include <server_mp/server_mp.h>
#include <game_mp/g_public_mp.h>
#include <dynentity/DynEntity_client.h>
#include "khz_host.h"
#include <atomic>

namespace khz_server_collision_load_detail {
using namespace khz::server_collision_checkpoint;
struct Image {
    Bytes<sizeof(cm)> map;
    Bytes<sizeof(g_traceThreadInfo)> infos;
    Bytes<sizeof(g_box_brush)> brushes;
    Bytes<sizeof(g_box_model)> models;
    Bytes<sizeof(cbrush_t)> templateBrush;
    KHZ_ServerCheckpointFrame frame;
    Signature geometry;
    void* partitionImage;
    std::size_t partitionBytes;
    bool captured;
};
Image image;
clipMap_t boundMap;
TraceThreadInfo boundInfos[THREAD_CONTEXT_COUNT];
void* boundTLS[THREAD_CONTEXT_COUNT];
const unsigned char* boundHunk;
int boundHunkBytes;
bool registered, bound;
std::atomic<unsigned> queryDepth[THREAD_CONTEXT_COUNT + 1]{};
std::atomic<unsigned> enrolledGroups{0};
std::atomic<bool> pinnedQueries{false}, foreignQueryDenied{false};
std::atomic<unsigned> deniedAttempts{0}, firstDeniedReason{0};
void markDenied(unsigned reason) {
    auto count = deniedAttempts.load(std::memory_order_relaxed);
    while (count != UINT32_MAX && !deniedAttempts.compare_exchange_weak(count, count + 1, std::memory_order_relaxed)) {}
    unsigned empty = 0; firstDeniedReason.compare_exchange_strong(empty, reason, std::memory_order_release);
    foreignQueryDenied.store(true, std::memory_order_release);
}

bool geometryFingerprint(Signature* signature, char* reason, unsigned capacity) {
    if (!cm.isInUse || !cm.name || cm.planeCount <= 0 || !cm.numNodes || !cm.numLeafs ||
        !cm.numSubModels || cm.numSubModels > MaxSubmodels || cm.numStaticModels > MaxStaticModels ||
        cm.triCount < 0 || cm.borderCount < 0 || cm.partitionCount < 0 ||
        unsigned(cm.partitionCount) > MaxPartitions || cm.aabbTreeCount < 0 ||
        cm.numClusters < 0 || cm.clusterBytes < 0 || !cm.box_brush ||
        !readable(cm.box_brush, sizeof(cbrush_t)))
        return reject(reason, capacity, "server_collision_invalid_or_unsupported_loaded_map_counts");
    Fingerprint hash; hash.begin();
    const auto array = [&](const void* p, std::size_t n, std::size_t size) { return hash.array(p, n, size); };
    if (!hash.append(&cm, sizeof(cm)) || !hash.string(cm.name) ||
        !array(cm.planes, unsigned(cm.planeCount), sizeof(*cm.planes)) ||
        !array(cm.materials, cm.numMaterials, sizeof(*cm.materials)) ||
        !array(cm.brushsides, cm.numBrushSides, sizeof(*cm.brushsides)) ||
        !array(cm.brushEdges, cm.numBrushEdges, 1) || !array(cm.nodes, cm.numNodes, sizeof(*cm.nodes)) ||
        !array(cm.leafs, cm.numLeafs, sizeof(*cm.leafs)) ||
        !array(cm.leafbrushNodes, cm.leafbrushNodesCount, sizeof(*cm.leafbrushNodes)) ||
        !array(cm.leafbrushes, cm.numLeafBrushes, sizeof(*cm.leafbrushes)) ||
        !array(cm.leafsurfaces, cm.numLeafSurfaces, sizeof(*cm.leafsurfaces)) ||
        !array(cm.verts, cm.vertCount, sizeof(*cm.verts)) ||
        unsigned(cm.triCount) > SingleSpanLimit / (3 * sizeof(*cm.triIndices)) ||
        !array(cm.triIndices, std::size_t(cm.triCount) * 3, sizeof(*cm.triIndices)) ||
        !array(cm.triEdgeIsWalkable, (std::size_t(cm.triCount) * 3 + 7) / 8, 1) ||
        !array(cm.borders, unsigned(cm.borderCount), sizeof(*cm.borders)) ||
        !array(cm.partitions, unsigned(cm.partitionCount), sizeof(*cm.partitions)) ||
        !array(cm.aabbTrees, unsigned(cm.aabbTreeCount), sizeof(*cm.aabbTrees)) ||
        !array(cm.cmodels, cm.numSubModels, sizeof(*cm.cmodels)) ||
        !array(cm.brushes, cm.numBrushes, sizeof(*cm.brushes)) ||
        !hash.append(cm.box_brush, sizeof(cbrush_t)) ||
        (cm.clusterBytes && unsigned(cm.numClusters) > SingleSpanLimit / unsigned(cm.clusterBytes)) ||
        !array(cm.visibility, std::size_t(cm.numClusters) * unsigned(cm.clusterBytes), 1))
        return reject(reason, capacity, "server_collision_geometry_span_unreadable_oversize_or_unpinned");
    // Static models are separately captured with their mutable writable links.
    // Definition-only DynEntity records are immutable map assets. Any actual
    // client/pose/collision arena is unsupported until it has a real provider.
    for (unsigned type = 0; type < 2; ++type) {
        if (cm.dynEntPoseList[type] || cm.dynEntClientList[type] || cm.dynEntCollList[type])
            return reject(reason, capacity, "server_collision_native_dynentity_mutable_arena_has_no_provider");
        if (!array(cm.dynEntDefList[type], cm.dynEntCount[type], sizeof(DynEntityDef)))
            return reject(reason, capacity, "server_collision_dynentity_definition_span_not_pinned");
    }
    if (!KHZ_ServerCollisionDynWorldIsEmpty())
        return reject(reason, capacity, "server_collision_native_dynentity_sector_arena_has_no_provider");
    if (!cm.mapEnts || !hash.append(cm.mapEnts, sizeof(*cm.mapEnts)) ||
        cm.mapEnts->numEntityChars <= 0 || !hash.string(cm.mapEnts->name) ||
        !array(cm.mapEnts->entityString, unsigned(cm.mapEnts->numEntityChars), 1))
        return reject(reason, capacity, "server_collision_original_map_entity_asset_is_unreadable");
    // Validate pointer-bearing BSP tables before any query could walk them.
    for (unsigned i = 0; i < cm.numNodes; ++i) {
        if (!element(cm.nodes[i].plane, cm.planes, unsigned(cm.planeCount)))
            return reject(reason, capacity, "server_collision_bsp_node_plane_is_foreign");
        for (int child : cm.nodes[i].children)
            if ((child >= 0 && unsigned(child) >= cm.numNodes) ||
                (child < 0 && unsigned(-1 - child) >= cm.numLeafs))
                return reject(reason, capacity, "server_collision_bsp_node_child_is_out_of_range");
    }
    const auto validLeaf = [&](const cLeaf_t& leaf) {
        return leaf.leafBrushNode >= 0 && unsigned(leaf.leafBrushNode) < cm.leafbrushNodesCount &&
            leaf.firstCollAabbIndex <= unsigned(cm.aabbTreeCount) &&
            leaf.collAabbCount <= unsigned(cm.aabbTreeCount) - leaf.firstCollAabbIndex;
    };
    for (unsigned i = 0; i < cm.numLeafs; ++i)
        if (!validLeaf(cm.leafs[i]))
            return reject(reason, capacity, "server_collision_bsp_leaf_query_indices_are_out_of_range");
    for (unsigned i = 1; i < cm.leafbrushNodesCount; ++i) {
        const auto& node = cm.leafbrushNodes[i];
        if (node.leafBrushCount > 0) {
            // Fastfile leaf lists may have separate immutable allocations.
            // Pin exactly the original list, not its enclosing DB/Hunk zone.
            if (!hash.array(node.data.leaf.brushes, unsigned(node.leafBrushCount), sizeof(uint16_t)))
                return reject(reason, capacity, "server_collision_bsp_leaf_brush_index_list_is_unpinned");
            for (int b = 0; b < node.leafBrushCount; ++b) {
                const auto index = node.data.leaf.brushes[b];
                const bool boxTemplate = int(i) == cm.box_model.leaf.leafBrushNode &&
                    node.leafBrushCount == 1 && index == cm.numBrushes;
                if (index >= cm.numBrushes && !boxTemplate)
                    return reject(reason, capacity, "server_collision_bsp_leaf_brush_index_is_out_of_range");
            }
        } else if (node.contents) {
            if (node.axis >= 3 || !std::isfinite(node.data.children.dist) ||
                !std::isfinite(node.data.children.range) || (node.leafBrushCount < 0 && i + 1 >= cm.leafbrushNodesCount))
                return reject(reason, capacity, "server_collision_bsp_leaf_brush_split_is_invalid");
            for (unsigned offset : node.data.children.childOffset)
                if (!offset || offset >= cm.leafbrushNodesCount - i)
                    return reject(reason, capacity, "server_collision_bsp_leaf_brush_child_is_out_of_range");
        }
    }
    for (unsigned i = 0; i < cm.numBrushSides; ++i)
        if (!element(cm.brushsides[i].plane, cm.planes, unsigned(cm.planeCount)))
            return reject(reason, capacity, "server_collision_brush_side_plane_is_foreign");
    for (unsigned i = 0; i < cm.numBrushes; ++i) {
        const auto& brush = cm.brushes[i];
        if (!bounds(brush.mins, brush.maxs) ||
            (brush.numsides && (!element(brush.sides, cm.brushsides, cm.numBrushSides) ||
                brush.numsides > cm.numBrushSides - unsigned(brush.sides - cm.brushsides))))
            return reject(reason, capacity, "server_collision_brush_geometry_or_sides_are_foreign");
    }
    for (unsigned i = 0; i < unsigned(cm.partitionCount); ++i) {
        const auto& partition = cm.partitions[i];
        if (partition.firstTri < 0 || partition.firstTri > cm.triCount ||
            partition.triCount > cm.triCount - partition.firstTri ||
            (partition.borderCount && (!element(partition.borders, cm.borders, unsigned(cm.borderCount)) ||
                partition.borderCount > unsigned(cm.borderCount) - unsigned(partition.borders - cm.borders))))
            return reject(reason, capacity, "server_collision_partition_triangle_or_border_span_is_invalid");
    }
    for (unsigned i = 0; i < cm.numSubModels; ++i)
        if (!bounds(cm.cmodels[i].mins, cm.cmodels[i].maxs) || !validLeaf(cm.cmodels[i].leaf))
            return reject(reason, capacity, "server_collision_loaded_brush_model_bounds_are_invalid");
    for (int i = 0; i < cm.triCount * 3; ++i)
        if (cm.triIndices[i] >= cm.vertCount)
            return reject(reason, capacity, "server_collision_triangle_vertex_index_is_out_of_range");
    for (int i = 0; i < cm.aabbTreeCount; ++i) {
        const auto& tree = cm.aabbTrees[i];
        if (!finite3(tree.origin) || !finite3(tree.halfSize) || tree.halfSize[0] < 0 ||
            tree.halfSize[1] < 0 || tree.halfSize[2] < 0 || tree.materialIndex >= cm.numMaterials ||
            (tree.childCount ? tree.u.firstChildIndex <= i || tree.u.firstChildIndex > cm.aabbTreeCount ||
                tree.childCount > cm.aabbTreeCount - tree.u.firstChildIndex :
                tree.u.partitionIndex < 0 || tree.u.partitionIndex >= cm.partitionCount))
            return reject(reason, capacity, "server_collision_aabb_query_geometry_or_indices_are_invalid");
    }
    *signature = hash.value; return true;
}
bool scratchState(char* reason, unsigned capacity) {
    if (!bound || boundHunk != s_hunkData || boundHunkBytes != s_hunkTotal ||
        std::memcmp(&boundMap, &cm, sizeof(cm)))
        return reject(reason, capacity, "server_collision_original_map_table_or_hunk_binding_changed");
    for (unsigned t = 0; t < THREAD_CONTEXT_COUNT; ++t) {
        const auto& info = g_traceThreadInfo[t];
        const auto& original = boundInfos[t];
        if (info.checkcount.partitions != original.checkcount.partitions ||
            info.box_brush != original.box_brush || info.box_model != original.box_model ||
            g_threadValues[t][3] != boundTLS[t])
            return reject(reason, capacity, "server_collision_trace_tls_or_original_scratch_pointer_changed");
        const bool initialized = t < THREAD_CONTEXT_TRACE_COUNT || t == THREAD_CONTEXT_CLIENT_SIM;
        if (!initialized) {
            if (info.checkcount.partitions || info.box_brush || info.box_model || info.checkcount.global)
                return reject(reason, capacity, "server_collision_unsupported_trace_context_has_mutable_storage");
            continue;
        }
        const auto bytes = std::size_t(cm.partitionCount) * sizeof(int);
        if (info.box_brush != &g_box_brush[t] || info.box_model != &g_box_model[t] ||
            (bytes && (!inside(info.checkcount.partitions, boundHunk, unsigned(boundHunkBytes), bytes) ||
                       !readable(info.checkcount.partitions, bytes))))
            return reject(reason, capacity, "server_collision_trace_scratch_is_not_original_hunk_storage");
        cbrush_t current = *info.box_brush, prototype = *cm.box_brush;
        std::memset(current.mins, 0, sizeof(current.mins)); std::memset(current.maxs, 0, sizeof(current.maxs)); current.contents = 0;
        std::memset(prototype.mins, 0, sizeof(prototype.mins)); std::memset(prototype.maxs, 0, sizeof(prototype.maxs)); prototype.contents = 0;
        if (std::memcmp(&current, &prototype, sizeof(current)) ||
            std::memcmp(&info.box_model->leaf, &cm.box_model.leaf, sizeof(cLeaf_t)) ||
            info.box_model->radius != cm.box_model.radius)
            return reject(reason, capacity, "server_collision_capsule_scratch_geometry_template_changed");
    }
    return true;
}
bool boundary(const KHZ_ServerCheckpointFrame& f, char* reason, unsigned capacity) {
    if (!context(reason, capacity) || !KHZ_ServerCollisionIdle(reason, capacity)) return false;
    if (!pinnedQueries.load(std::memory_order_seq_cst))
        return reject(reason, capacity, "server_collision_all_provider_query_fence_is_missing");
    if (!com_dedicated || !com_dedicated->current.integer || sv.state != SS_GAME || sv.inFrame || sv.restarting ||
        f.mapChecksum != cm.checksum || f.hunkBaseAddress != reinterpret_cast<std::uintptr_t>(s_hunkData) ||
        f.serverAddress != reinterpret_cast<std::uintptr_t>(&sv) || f.entitiesAddress != reinterpret_cast<std::uintptr_t>(g_entities) ||
        Sys_GetValue(3) != &g_traceThreadInfo[THREAD_CONTEXT_MAIN])
        return reject(reason, capacity, "server_collision_requires_original_completed_dedicated_map_and_main_tls");
    return scratchState(reason, capacity);
}
bool capture(const KHZ_ServerCheckpointFrame& f, char* reason, unsigned capacity) {
    if (!boundary(f, reason, capacity) || image.partitionImage) return false;
    if (!geometryFingerprint(&image.geometry, reason, capacity)) return false;
    image.partitionBytes = std::size_t(cm.partitionCount) * sizeof(int);
    if (image.partitionBytes) {
        image.partitionImage = VirtualAlloc(nullptr, image.partitionBytes * THREAD_CONTEXT_COUNT, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!image.partitionImage) return reject(reason, capacity, "server_collision_partition_image_allocation_failed");
        for (unsigned t = 0; t < THREAD_CONTEXT_COUNT; ++t) if (boundInfos[t].checkcount.partitions)
            std::memcpy(static_cast<unsigned char*>(image.partitionImage) + t * image.partitionBytes,
                boundInfos[t].checkcount.partitions, image.partitionBytes);
    }
    image.map.capture(&cm); image.infos.capture(g_traceThreadInfo);
    image.brushes.capture(g_box_brush); image.models.capture(g_box_model); image.templateBrush.capture(cm.box_brush);
    image.frame = f; image.captured = true; return true;
}
bool validate(const KHZ_ServerCheckpointFrame& f, char* reason, unsigned capacity) {
    if (!image.captured || !khz::server_checkpoint::sameLifetime(image.frame, f))
        return reject(reason, capacity, "server_collision_original_map_or_paused_image_lifetime_changed");
    if (!boundary(f, reason, capacity)) return false;
    Signature current;
    if (!geometryFingerprint(&current, reason, capacity)) return false;
    return same(current, image.geometry) ? true : reject(reason, capacity, "server_collision_pinned_bsp_or_asset_geometry_changed");
}
void restore(const KHZ_ServerCheckpointFrame&) {
    iassert(KHZ_ServerCheckpointContext() && image.captured);
    image.map.restore(&cm); image.infos.restore(g_traceThreadInfo);
    image.brushes.restore(g_box_brush); image.models.restore(g_box_model); image.templateBrush.restore(cm.box_brush);
    for (unsigned t = 0; t < THREAD_CONTEXT_COUNT; ++t) if (image.partitionBytes && boundInfos[t].checkcount.partitions)
        std::memcpy(boundInfos[t].checkcount.partitions,
            static_cast<const unsigned char*>(image.partitionImage) + t * image.partitionBytes, image.partitionBytes);
}
void discard() {
    image.captured = false; // Root releases the fence after ALL images retire.
    if (image.partitionImage) VirtualFree(image.partitionImage, 0, MEM_RELEASE);
    image.partitionImage = nullptr; image.partitionBytes = 0;
}
}

void KHZ_ServerCollisionLoadRegisterCheckpoint() {
    using namespace khz_server_collision_load_detail;
    if (registered) Com_Error(ERR_FATAL, "Duplicate server collision load checkpoint provider");
    registerPart({KHZ_ServerCheckpointDomain::Collision, 0,
        sizeof(image) + std::size_t(MaxPartitions) * sizeof(int) * THREAD_CONTEXT_COUNT,
        capture, validate, restore, discard});
    if (!KHZ_ServerCheckpointRegisterRange(KHZ_ServerCheckpointDomain::Collision, &cm, sizeof(cm)) ||
        !KHZ_ServerCheckpointRegisterRange(KHZ_ServerCheckpointDomain::Collision, g_traceThreadInfo, sizeof(g_traceThreadInfo)) ||
        !KHZ_ServerCheckpointRegisterRange(KHZ_ServerCheckpointDomain::Collision, g_box_brush, sizeof(g_box_brush)) ||
        !KHZ_ServerCheckpointRegisterRange(KHZ_ServerCheckpointDomain::Collision, g_box_model, sizeof(g_box_model)))
        Com_Error(ERR_FATAL, "Could not register exact collision map/TLS/box BSS spans");
    registered = true;
}
bool KHZ_ServerCollisionBindMap(char* reason, unsigned capacity) {
    using namespace khz_server_collision_load_detail;
    if (!registered || !Sys_IsMainThread() || !com_dedicated || !com_dedicated->current.integer ||
        KHZ_ServerCheckpointPinned() || !s_hunkData || s_hunkTotal <= 0 || !cm.isInUse ||
        !KHZ_ServerCollisionIdle(reason, capacity))
        return reject(reason, capacity, "server_collision_map_bind_requires_enrolled_unpinned_idle_dedicated_owner");
    bound = false; boundMap = cm; boundHunk = s_hunkData; boundHunkBytes = s_hunkTotal;
    std::memcpy(boundInfos, g_traceThreadInfo, sizeof(boundInfos));
    for (unsigned t = 0; t < THREAD_CONTEXT_COUNT; ++t) boundTLS[t] = g_threadValues[t][3];
    bound = true;
    Signature geometry;
    if (!scratchState(reason, capacity) || !geometryFingerprint(&geometry, reason, capacity)) { bound = false; return false; }
    const void* bases[THREAD_CONTEXT_COUNT + 2]; std::size_t bytes[THREAD_CONTEXT_COUNT + 2]; unsigned count = 0;
    for (unsigned t = 0; t < THREAD_CONTEXT_COUNT; ++t) if (cm.partitionCount && boundInfos[t].checkcount.partitions) {
        bases[count] = boundInfos[t].checkcount.partitions; bytes[count++] = std::size_t(cm.partitionCount) * sizeof(int);
        for (unsigned i = 0; i + 1 < count; ++i) if (bases[i] == bases[count - 1]) {
            bound = false; return reject(reason, capacity, "server_collision_partition_context_allocations_are_aliased");
        }
    }
    bases[count] = cm.box_brush; bytes[count++] = sizeof(cbrush_t);
    const void* worldBase; std::size_t worldBytes;
    if (!KHZ_ServerCollisionWorldMapSpan(&worldBase, &worldBytes, reason, capacity)) { bound = false; return false; }
    if (worldBytes) { bases[count] = worldBase; bytes[count++] = worldBytes; }
    if (!KHZ_ServerCheckpointReplaceMapRanges(KHZ_ServerCheckpointDomain::Collision, bases, bytes, count)) {
        bound = false; return reject(reason, capacity, "server_collision_exact_map_restore_range_replacement_failed");
    }
    firstDeniedReason.store(0, std::memory_order_release);
    foreignQueryDenied.store(false, std::memory_order_release); return true;
}
bool KHZ_ServerCollisionInvalidateMapBinding() {
    using namespace khz_server_collision_load_detail;
    if (!Sys_IsMainThread()) { markDenied(2); return false; }
    if (!KHZ_ServerCollisionAllowUnsupportedEffect("server_collision_map_binding_release")) return false;
    if (registered && !KHZ_ServerCheckpointReplaceMapRanges(KHZ_ServerCheckpointDomain::Collision, nullptr, nullptr, 0))
        return false;
    bound = false; boundHunk = nullptr; boundHunkBytes = 0;
    return true;
}
void KHZ_ServerCollisionEnrollQueryHooks(unsigned groups) {
    using namespace khz_server_collision_load_detail;
    iassert(Sys_IsMainThread() && !KHZ_ServerCheckpointPinned());
    if (!groups || (groups & ~KHZ_ServerCollisionRequiredParts)) Com_Error(ERR_FATAL, "Invalid collision query guard enrollment");
    enrolledGroups.fetch_or(groups, std::memory_order_release);
}
bool KHZ_ServerCollisionIdle(char* reason, unsigned capacity) {
    using namespace khz_server_collision_load_detail;
    if (!Sys_IsMainThread() || enrolledGroups.load(std::memory_order_acquire) != KHZ_ServerCollisionRequiredParts)
        return reject(reason, capacity, "server_collision_native_query_guards_not_enrolled");
    for (const auto& depth : queryDepth) if (depth.load(std::memory_order_seq_cst))
        return reject(reason, capacity, "server_collision_native_trace_or_link_query_still_active");
    if (foreignQueryDenied.load(std::memory_order_acquire) && !KHZ_ServerCheckpointPinned() &&
        pinnedQueries.load(std::memory_order_seq_cst))
        return reject(reason, capacity, "server_collision_foreign_attempt_during_capture_fence");
    if (KHZ_ServerCheckpointPinned() && foreignQueryDenied.exchange(false, std::memory_order_acq_rel)) {
        const unsigned denied = firstDeniedReason.load(std::memory_order_acquire);
        KHZ_ServerCheckpointRejectExternalEffect(denied == 2 ? "server_collision_unsupported_foreign_effect_denied_before_mutation" :
            denied == 3 ? "server_collision_native_query_aborted_by_engine_error" : "server_collision_foreign_query_owner_denied_before_effect");
    }
    return true;
}
const char* KHZ_ServerCollisionStatusJSON() {
    using namespace khz_server_collision_load_detail;
    iassert(Sys_IsMainThread());
    static char text[768];
    std::snprintf(text, sizeof(text),
        "{\"staged\":true,\"required_parts\":7,\"enrolled_query_groups\":%u,\"query_fence\":%s,\"map_bound\":%s,\"image_captured\":%s,"
        "\"partition_count\":%d,\"partition_image_bytes\":%llu,\"native_dynentity_mutable\":%s,\"foreign_denied_attempts\":%u,\"first_denied_reason\":%u,"
        "\"active_queries\":[%u,%u,%u,%u,%u,%u,%u,%u,%u],\"full_world_replay_verified\":false}",
        enrolledGroups.load(), pinnedQueries.load() ? "true" : "false", bound ? "true" : "false", image.captured ? "true" : "false",
        cm.partitionCount, static_cast<unsigned long long>(image.partitionImage ? image.partitionBytes * THREAD_CONTEXT_COUNT : 0),
        (cm.dynEntPoseList[0] || cm.dynEntPoseList[1] || cm.dynEntClientList[0] || cm.dynEntClientList[1] ||
            cm.dynEntCollList[0] || cm.dynEntCollList[1]) ? "true" : "false", deniedAttempts.load(), firstDeniedReason.load(),
        queryDepth[0].load(), queryDepth[1].load(), queryDepth[2].load(), queryDepth[3].load(), queryDepth[4].load(),
        queryDepth[5].load(), queryDepth[6].load(), queryDepth[7].load(), queryDepth[8].load());
    return text;
}
bool KHZ_ServerCollisionFenceEnter(char* reason, unsigned capacity) {
    using namespace khz_server_collision_load_detail;
    if (!Sys_IsMainThread() || !com_dedicated || !com_dedicated->current.integer || !KHZ_ServerCheckpointPaused())
        return reject(reason, capacity, "server_collision_query_fence_requires_actual_paused_dedicated_owner");
    pinnedQueries.store(true, std::memory_order_seq_cst);
    return KHZ_ServerCollisionIdle(reason, capacity);
}
void KHZ_ServerCollisionFenceRelease() {
    using namespace khz_server_collision_load_detail;
    iassert(Sys_IsMainThread() && !KHZ_ServerCheckpointPinned());
    if (KHZ_ServerCheckpointPinned()) return;
    pinnedQueries.store(false, std::memory_order_seq_cst);
}
bool KHZ_ServerCollisionQueryBegin(unsigned group, int* slot) {
    using namespace khz_server_collision_load_detail;
    if (!slot || !group || (group & ~KHZ_ServerCollisionRequiredParts)) return false;
    *slot = THREAD_CONTEXT_COUNT;
    const bool main = Sys_IsMainThread();
    bool nativeTLS = main;
    if (!main && !pinnedQueries.load(std::memory_order_seq_cst)) {
        const auto id = Sys_GetCurrentThreadId();
        for (unsigned t = 0; t < THREAD_CONTEXT_COUNT; ++t) nativeTLS = nativeTLS || (threadId[t] && threadId[t] == id);
    }
    // Anonymous SDK/OS workers do not have g_threadLocals. Never dereference
    // their TLS merely to reject an unsupported query during a pinned branch.
    const auto* info = nativeTLS ? static_cast<const TraceThreadInfo*>(Sys_GetValue(3)) : nullptr;
    if (element(info, g_traceThreadInfo, THREAD_CONTEXT_COUNT)) *slot = int(info - g_traceThreadInfo);
    const bool denied = pinnedQueries.load(std::memory_order_seq_cst) && (!main || *slot != THREAD_CONTEXT_MAIN);
    if (denied) {
        markDenied(1);
        if (Sys_IsMainThread()) KHZ_ServerCheckpointRejectExternalEffect("server_collision_foreign_query_owner_denied_before_effect");
        *slot = -1; return false;
    }
    queryDepth[*slot].fetch_add(1, std::memory_order_seq_cst);
    // Gate store, counter increment, gate recheck and observer counter loads
    // share one SC total order. If the recheck precedes publication, the active
    // counter is visible to capture until QueryEnd. If publication precedes
    // recheck, this query is refused before its first native read/write.
    if (!main && pinnedQueries.load(std::memory_order_seq_cst)) {
        queryDepth[*slot].fetch_sub(1, std::memory_order_seq_cst);
        markDenied(1); *slot = -1; return false;
    }
    return true;
}
void KHZ_ServerCollisionQueryEnd(int slot) {
    using namespace khz_server_collision_load_detail;
    iassert(slot >= 0 && slot <= THREAD_CONTEXT_COUNT);
    const auto previous = queryDepth[slot].fetch_sub(1, std::memory_order_seq_cst); iassert(previous != 0);
}
void KHZ_ServerCollisionBlockedTrace(trace_t* result, int contents) {
    if (!result) return;
    std::memset(result, 0, sizeof(*result));
    result->fraction = 0.0f;
    result->contents = contents;
    result->allsolid = result->startsolid = true;
    result->hitType = TRACE_HITTYPE_ENTITY;
    result->hitId = ENTITYNUM_WORLD;
}
void KHZ_ServerCollisionErrorCleanup() {
    using namespace khz_server_collision_load_detail;
    iassert(Sys_IsMainThread());
    if (queryDepth[THREAD_CONTEXT_MAIN].exchange(0, std::memory_order_seq_cst)) {
        markDenied(3);
        if (KHZ_ServerCheckpointPinned()) KHZ_ServerCheckpointRejectExternalEffect("server_collision_native_query_aborted_by_engine_error");
    }
}
bool KHZ_ServerCollisionAllowUnsupportedEffect(const char* operation) {
    using namespace khz_server_collision_load_detail;
    if (!pinnedQueries.load(std::memory_order_seq_cst)) return true;
    if (!Sys_IsMainThread()) { markDenied(2); return false; }
    KHZ_ServerCheckpointRejectExternalEffect(operation ? operation : "server_collision_unsupported_native_geometry_or_asset_effect"); return false;
}

