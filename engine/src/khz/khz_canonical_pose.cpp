#include "khz_canonical.h"
#include "khz_client_owner.h"
#include "khz_client_pose.h"
#include <universal/q_shared.h>
#include <client_mp/client_mp.h>
#include <cgame_mp/cg_local_mp.h>
#include <xanim/dobj.h>
#include <xanim/xanim.h>
#include <xanim/xmodel.h>
#include <script/scr_stringlist.h>
#include <cstdint>

// Narrow read-only arena views supplied by xanim.cpp at checkpoint56. They
// require the real simulation owner and copy one bounded native record.
bool KHZ_XAnimOwnerReadNode(unsigned index, XAnimInfo* output);
bool KHZ_XAnimOwnerReadNotification(unsigned index, XAnimNotify_s* output);
unsigned KHZ_XAnimOwnerNotificationCount();
bool KHZ_ClientProjectileCanonical(khz::canonical::Hash& hash);

namespace {
bool reject(khz::canonical::Hash& hash) { hash.reject(); return false; }
bool object(khz::canonical::Hash& hash, const DObj_s& value, const clientActive_t& client) {
    if (!value.models || !value.numModels || value.numModels > 9 || !value.numBones || value.numBones > 128 ||
        !value.duplicateParts || value.duplicatePartsSize < 17 || value.locked) return reject(hash);
    hash.scalar(value.entnum); hash.scalar(value.numModels); hash.scalar(value.numBones); hash.scalar(value.ignoreCollision);
    hash.scalar(value.radius); hash.array(value.hidePartBits);
    const auto* parents = reinterpret_cast<const unsigned char*>(&value.models[value.numModels]);
    for (unsigned i = 0; i < value.numModels; ++i) { hash.text(value.models[i]->name); hash.scalar(parents[i]); }
    const auto* duplicates = reinterpret_cast<const unsigned char*>(SL_ConvertToString(value.duplicateParts));
    hash.scalar(value.duplicatePartsSize);
    for (unsigned i = 0; i < value.duplicatePartsSize; ++i) hash.octet(duplicates[i]);
    hash.boolean(value.tree != nullptr);
    if (value.tree) {
        if (!value.tree->anims || value.tree->info_usage < 0 || value.tree->info_usage > 4096 ||
            value.tree->calcRefCount || value.tree->modifyRefCount) return reject(hash);
        hash.text(value.tree->anims->debugName); hash.scalar(value.tree->anims->size);
        hash.scalar(value.tree->children); hash.scalar(value.tree->info_usage);
    }
    // Old per-tick matrices point into a recycled scratch arena. Observe only
    // the valid current allocation and masks; stale bytes cannot affect replay.
    const bool current = value.skel.timeStamp == client.skelTimeStamp;
    hash.boolean(current);
    if (!current) return true;
    const auto start = reinterpret_cast<std::uintptr_t>(client.skelMemoryStart);
    const auto matrix = reinterpret_cast<std::uintptr_t>(value.skel.mat);
    if (!start || !matrix || matrix < start || matrix - start > sizeof(client.skelMemory) ||
        value.numBones * sizeof(DObjAnimMat) > sizeof(client.skelMemory) - (matrix - start)) return reject(hash);
    hash.array(value.skel.partBits.anim.array); hash.array(value.skel.partBits.control.array); hash.array(value.skel.partBits.skel.array);
    for (unsigned b = 0; b < value.numBones; ++b) {
        const bool initialized = value.skel.partBits.anim.testBit(b) || value.skel.partBits.control.testBit(b) || value.skel.partBits.skel.testBit(b);
        hash.boolean(initialized);
        if (initialized) { hash.array(value.skel.mat[b].quat); hash.array(value.skel.mat[b].trans); hash.scalar(value.skel.mat[b].transWeight); }
    }
    return true;
}
bool animation(khz::canonical::Hash& hash, const XAnimTree_s* const* trees, unsigned count) {
    unsigned allocated = 1; // native free-list sentinel at index zero
    for (unsigned i = 0; i < 4096; ++i) {
        XAnimInfo node{}; if (!KHZ_XAnimOwnerReadNode(i, &node)) return reject(hash);
        hash.scalar(i); hash.boolean(node.inuse); hash.scalar(node.prev); hash.scalar(node.next);
        if (!node.inuse) continue;
        ++allocated;
        unsigned handle = count;
        for (unsigned t = 0; t < count; ++t) if (trees[t] == node.tree) { handle = t; break; }
        if (handle == count || !node.tree || !node.tree->anims || node.animIndex >= node.tree->anims->size ||
            node.prev >= 4096 || node.next >= 4096 || node.children >= 4096 || node.parent >= 4096 || node.notifyChild >= 4096) return reject(hash);
        hash.scalar(handle); hash.scalar(node.animIndex); hash.scalar(node.children); hash.scalar(node.parent); hash.scalar(node.notifyChild);
        hash.scalar(node.notifyIndex); hash.text(node.notifyName ? SL_ConvertToString(node.notifyName) : nullptr); hash.scalar(node.notifyType);
        const bool leaf = XAnimIsLeafNode(node.tree->anims, node.animIndex); hash.boolean(leaf);
        if (leaf) hash.text(node.parts ? node.parts->name : nullptr);
        else { hash.scalar(node.animParent.flags); hash.scalar(node.animParent.children); }
        hash.boolean(node.animToModel != 0);
        if (node.animToModel) {
            const auto* map = reinterpret_cast<const unsigned char*>(SL_ConvertToString(node.animToModel));
            const unsigned bytes = unsigned(map[16]) + 17;
            if (bytes > 145) return reject(hash);
            hash.scalar(bytes); for (unsigned n = 0; n < bytes; ++n) hash.octet(map[n]);
        }
        hash.scalar(node.state.currentAnimTime); hash.scalar(node.state.oldTime); hash.scalar(node.state.cycleCount); hash.scalar(node.state.oldCycleCount);
        hash.scalar(node.state.goalTime); hash.scalar(node.state.goalWeight); hash.scalar(node.state.weight); hash.scalar(node.state.rate);
        hash.scalar(node.state.instantWeightChange);
    }
    if (XAnimGetTreeMemUsage() != static_cast<int>(allocated * sizeof(XAnimInfo))) return reject(hash);
    const unsigned notifications = KHZ_XAnimOwnerNotificationCount();
    if (notifications > 128) return reject(hash);
    hash.scalar(notifications);
    for (unsigned i = 0; i < notifications; ++i) {
        XAnimNotify_s notification{}; if (!KHZ_XAnimOwnerReadNotification(i, &notification)) return reject(hash);
        hash.text(notification.name); hash.scalar(notification.type); hash.scalar(notification.timeFrac);
    }
    return true;
}
}

bool KHZ_ClientPoseCanonical(khz::canonical::Hash& hash) {
    iassert(KHZ_IsClientSimulationThread());
    if (!KHZ_IsClientSimulationThread()) return reject(hash);
    const auto& client = *CL_GetLocalClientGlobals(0); const auto& game = *CG_GetLocalClientGlobals(0);
    const XAnimTree_s* trees[64]{};
    // Include actual private controllers and calculated native skeletons. No
    // corresponding render DObj or immutable presentation sample is inspected.
    for (unsigned i = 0; i < 1024; ++i) {
        const auto* body = KHZ_ClientPoseGetDObj(i);
        if (!body) continue;
        const auto& entity = *CG_GetEntity(0, i); hash.scalar(i);
        if (!object(hash, *body, client)) return false;
        hash.array(entity.pose.origin); hash.array(entity.pose.angles); hash.scalar(entity.previousEventSequence);
        entityState_s current = entity.nextState; current.lerp = entity.currentState;
        KHZ_CanonicalEntity(hash, current); KHZ_CanonicalEntity(hash, entity.nextState);
        if (i < 64) {
            hash.array(entity.pose.player.tag); KHZ_CanonicalController(hash, game.bgs.clientinfo[i]); trees[i] = body->tree;
        }
    }
    return animation(hash, trees, 64) && KHZ_ClientProjectileCanonical(hash) && hash.finish().valid;
}
