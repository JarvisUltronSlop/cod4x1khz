// Source-owned fragment: END of qcommon/cm_world.cpp, opt-in custom MP/dedicated.
// cm_world_t/worldSector_s are private native types intentionally left native.
#include "khz_server_collision_checkpoint_util.h"
#include <qcommon/threads.h>
#include <server_mp/server_mp.h>
#include <game_mp/g_public_mp.h>
#include <xanim/xmodel.h>

namespace khz_server_collision_world_detail {
using namespace khz::server_collision_checkpoint;
struct Image {
    Bytes<sizeof(cm_world)> sectors;
    Bytes<sizeof(sv.svEntities)> entities;
    KHZ_ServerCheckpointFrame frame;
    Signature models;
    cStaticModel_s* liveModels;
    void* modelImage;
    std::size_t modelBytes;
    unsigned modelCount;
    bool captured;
};
Image image;
// Private bounded validation workspace, never queried by native gameplay.
unsigned char sectorSeen[1024], entitySeen[1024], modelSeen[MaxStaticModels];
unsigned sectorStack[1024];
const XModel* modelAssets[4096];
unsigned modelAssetCount;

bool modelFingerprint(Signature* signature, char* reason, unsigned capacity) {
    if (cm.numStaticModels > MaxStaticModels ||
        (cm.numStaticModels && !readable(cm.staticModelList, std::size_t(cm.numStaticModels) * sizeof(cStaticModel_s))))
        return reject(reason, capacity, "server_collision_static_model_records_are_unreadable_or_oversize");
    Fingerprint hash; hash.begin(); modelAssetCount = 0;
    for (unsigned i = 0; i < cm.numStaticModels; ++i) {
        const auto& model = cm.staticModelList[i];
        if (!bounds(model.absmin, model.absmax) || !finite3(model.origin) ||
            !finite3(model.invScaledAxis[0]) || !finite3(model.invScaledAxis[1]) || !finite3(model.invScaledAxis[2]) ||
            !model.xmodel || !readable(model.xmodel, sizeof(XModel)) ||
            !hash.append(reinterpret_cast<const unsigned char*>(&model) + offsetof(cStaticModel_s, xmodel),
                sizeof(cStaticModel_s) - offsetof(cStaticModel_s, xmodel)))
            return reject(reason, capacity, "server_collision_static_model_transform_or_asset_is_invalid");
        bool found = false;
        for (unsigned a = 0; a < modelAssetCount; ++a) found = found || modelAssets[a] == model.xmodel;
        if (found) continue;
        if (modelAssetCount == sizeof(modelAssets) / sizeof(modelAssets[0]))
            return reject(reason, capacity, "server_collision_static_model_unique_asset_capacity_exceeded");
        modelAssets[modelAssetCount++] = model.xmodel;
        const auto& asset = *model.xmodel;
        if (asset.numCollSurfs < 0 || asset.numCollSurfs > 4096 ||
            !hash.string(asset.name) || !hash.append(&asset.contents, sizeof(asset.contents)) ||
            !hash.append(&asset.collLod, sizeof(asset.collLod)) ||
            !hash.append(&asset.collSurfs, sizeof(asset.collSurfs)) ||
            !hash.append(&asset.numCollSurfs, sizeof(asset.numCollSurfs)) ||
            !hash.array(asset.collSurfs, unsigned(asset.numCollSurfs), sizeof(XModelCollSurf_s)))
            return reject(reason, capacity, "server_collision_static_xmodel_collision_asset_is_unpinned");
        for (int s = 0; s < asset.numCollSurfs; ++s) {
            const auto& surface = asset.collSurfs[s];
            if (surface.numCollTris < 0 || !bounds(surface.mins, surface.maxs) ||
                !hash.array(surface.collTris, unsigned(surface.numCollTris), sizeof(XModelCollTri_s)))
                return reject(reason, capacity, "server_collision_static_xmodel_triangle_geometry_is_unpinned");
        }
    }
    *signature = hash.value; return true;
}
bool topology(char* reason, unsigned capacity) {
    if (!bounds(cm_world.mins, cm_world.maxs) || cm_world.freeHead >= 1024 ||
        sv.gentities != g_entities || sv.gentitySize != sizeof(gentity_s) ||
        sv.num_entities < 0 || sv.num_entities > 1024 || cm.numStaticModels > MaxStaticModels)
        return reject(reason, capacity, "server_collision_world_bounds_or_original_entity_arena_invalid");
    std::memset(sectorSeen, 0, sizeof(sectorSeen)); std::memset(entitySeen, 0, sizeof(entitySeen));
    std::memset(modelSeen, 0, sizeof(modelSeen));
    unsigned pending = 1; sectorStack[0] = SECTOR_HEAD;
    while (pending) {
        const unsigned index = sectorStack[--pending];
        if (!index || index >= 1024 || sectorSeen[index])
            return reject(reason, capacity, "server_collision_sector_tree_cycle_or_invalid_index");
        sectorSeen[index] = 1;
        const auto& sector = cm_world.sectors[index];
        if (sector.tree.axis >= 2 || !std::isfinite(sector.tree.dist) ||
            (index == SECTOR_HEAD && sector.tree.u.parent))
            return reject(reason, capacity, "server_collision_sector_axis_distance_or_root_parent_invalid");
        for (unsigned child : sector.tree.child) if (child) {
            if (child >= 1024 || child == SECTOR_HEAD || cm_world.sectors[child].tree.u.parent != index || pending == 1024)
                return reject(reason, capacity, "server_collision_sector_parent_child_not_reciprocal");
            sectorStack[pending++] = child;
        }
        unsigned count = 0;
        for (unsigned entry = sector.contents.entities; entry; entry = sv.svEntities[entry - 1].nextEntityInWorldSector) {
            if (entry > 1024 || ++count > 1024 || entitySeen[entry - 1])
                return reject(reason, capacity, "server_collision_entity_link_cycle_duplicate_or_invalid_index");
            entitySeen[entry - 1] = 1;
            const auto& linked = sv.svEntities[entry - 1]; const auto& entity = g_entities[entry - 1];
            if (linked.worldSector != index || !entity.r.inuse || !entity.r.linked || !entity.r.contents ||
                linked.numClusters < 0 || linked.numClusters > 16 ||
                !std::isfinite(linked.linkmin[0]) || !std::isfinite(linked.linkmin[1]) ||
                !std::isfinite(linked.linkmax[0]) || !std::isfinite(linked.linkmax[1]) ||
                linked.linkmin[0] > linked.linkmax[0] || linked.linkmin[1] > linked.linkmax[1])
                return reject(reason, capacity, "server_collision_entity_sector_binding_or_cached_bounds_invalid");
            for (int c = 0; c < linked.numClusters; ++c)
                if (linked.clusternums[c] < 0 || linked.clusternums[c] >= cm.numClusters)
                    return reject(reason, capacity, "server_collision_entity_pvs_cluster_cache_out_of_range");
        }
        count = 0;
        for (unsigned entry = sector.contents.staticModels; entry; entry = cm.staticModelList[entry - 1].writable.nextModelInWorldSector) {
            if (entry > cm.numStaticModels || ++count > cm.numStaticModels || modelSeen[entry - 1])
                return reject(reason, capacity, "server_collision_static_model_link_cycle_duplicate_or_invalid_index");
            modelSeen[entry - 1] = 1;
        }
    }
    unsigned freeCount = 0;
    for (unsigned index = cm_world.freeHead; index; index = cm_world.sectors[index].tree.u.nextFree) {
        if (index >= 1024 || index == SECTOR_HEAD || sectorSeen[index] || ++freeCount > 1023)
            return reject(reason, capacity, "server_collision_sector_free_chain_cycle_or_overlap");
        sectorSeen[index] = 2;
        const auto& sector = cm_world.sectors[index];
        if (sector.contents.entities || sector.contents.staticModels || sector.contents.contentsStaticModels ||
            sector.contents.contentsEntities || sector.contents.linkcontentsEntities || sector.tree.child[0] || sector.tree.child[1])
            return reject(reason, capacity, "server_collision_free_sector_contains_live_links_or_contents");
    }
    for (unsigned i = 1; i < 1024; ++i) if (!sectorSeen[i])
        return reject(reason, capacity, "server_collision_sector_allocation_graph_has_unreachable_storage");
    for (unsigned i = 0; i < 1024; ++i) {
        const auto& linked = sv.svEntities[i];
        if (linked.worldSector >= 1024 || linked.nextEntityInWorldSector > 1024 ||
            bool(linked.worldSector) != bool(entitySeen[i]))
            return reject(reason, capacity, "server_collision_entity_link_membership_is_incomplete");
    }
    for (unsigned i = 0; i < cm.numStaticModels; ++i)
        if (cm.staticModelList[i].writable.nextModelInWorldSector > cm.numStaticModels ||
            bool(cm.staticModelList[i].xmodel->contents) != bool(modelSeen[i]))
            return reject(reason, capacity, "server_collision_static_model_link_membership_is_incomplete");
    return true;
}
bool boundary(const KHZ_ServerCheckpointFrame& f, char* reason, unsigned capacity) {
    return context(reason, capacity) && KHZ_ServerCollisionIdle(reason, capacity) &&
        (f.serverAddress == reinterpret_cast<std::uintptr_t>(&sv) &&
         f.entitiesAddress == reinterpret_cast<std::uintptr_t>(g_entities) &&
         f.mapChecksum == cm.checksum && sv.state == SS_GAME && !sv.inFrame && !sv.restarting ? true :
         reject(reason, capacity, "server_collision_world_not_original_completed_map"));
}
bool capture(const KHZ_ServerCheckpointFrame& f, char* reason, unsigned capacity) {
    if (!boundary(f, reason, capacity) || image.modelImage) return false;
    if (!modelFingerprint(&image.models, reason, capacity) || !topology(reason, capacity)) return false;
    image.modelBytes = std::size_t(cm.numStaticModels) * sizeof(cStaticModel_s);
    if (image.modelBytes) {
        image.modelImage = VirtualAlloc(nullptr, image.modelBytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!image.modelImage) return reject(reason, capacity, "server_collision_static_model_image_allocation_failed");
        std::memcpy(image.modelImage, cm.staticModelList, image.modelBytes);
    }
    image.sectors.capture(&cm_world); image.entities.capture(sv.svEntities);
    image.liveModels = cm.staticModelList; image.modelCount = cm.numStaticModels;
    image.frame = f; image.captured = true; return true;
}
bool validate(const KHZ_ServerCheckpointFrame& f, char* reason, unsigned capacity) {
    if (!image.captured || !khz::server_checkpoint::sameLifetime(image.frame, f) ||
        image.liveModels != cm.staticModelList || image.modelCount != cm.numStaticModels)
        return reject(reason, capacity, "server_collision_world_or_static_model_original_binding_changed");
    if (!boundary(f, reason, capacity)) return false;
    Signature current;
    if (!modelFingerprint(&current, reason, capacity) || !topology(reason, capacity)) return false;
    return same(current, image.models) ? true : reject(reason, capacity, "server_collision_pinned_static_model_transform_or_collision_asset_changed");
}
void restore(const KHZ_ServerCheckpointFrame&) {
    iassert(KHZ_ServerCheckpointContext() && image.captured);
    image.sectors.restore(&cm_world); image.entities.restore(sv.svEntities);
    if (image.modelBytes) std::memcpy(image.liveModels, image.modelImage, image.modelBytes);
}
void discard() {
    image.captured = false;
    if (image.modelImage) VirtualFree(image.modelImage, 0, MEM_RELEASE);
    image.modelImage = nullptr; image.modelBytes = 0; image.liveModels = nullptr; image.modelCount = 0;
}
}
void KHZ_ServerCollisionWorldRegisterCheckpoint() {
    using namespace khz_server_collision_world_detail;
    registerPart({KHZ_ServerCheckpointDomain::Collision, 1,
        sizeof(image) + std::size_t(MaxStaticModels) * sizeof(cStaticModel_s), capture, validate, restore, discard});
    if (!KHZ_ServerCheckpointRegisterRange(KHZ_ServerCheckpointDomain::Collision, &cm_world, sizeof(cm_world)) ||
        !KHZ_ServerCheckpointRegisterRange(KHZ_ServerCheckpointDomain::Collision, sv.svEntities, sizeof(sv.svEntities)))
        Com_Error(ERR_FATAL, "Could not register exact native collision sector/entity cache spans");
}
bool KHZ_ServerCollisionWorldMapSpan(const void** base, std::size_t* bytes, char* reason, unsigned capacity) {
    using namespace khz_server_collision_world_detail;
    if (!base || !bytes || !Sys_IsMainThread() || KHZ_ServerCheckpointPinned() || cm.numStaticModels > MaxStaticModels)
        return reject(reason, capacity, "server_collision_static_model_map_range_bind_requires_original_unpinned_owner");
    *base = cm.staticModelList; *bytes = std::size_t(cm.numStaticModels) * sizeof(cStaticModel_s);
    return !*bytes || readable(*base, *bytes) ? true : reject(reason, capacity, "server_collision_static_model_map_range_is_unreadable");
}

