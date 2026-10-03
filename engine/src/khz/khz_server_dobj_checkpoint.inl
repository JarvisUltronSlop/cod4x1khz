// Source-owned seam: include at the END of qcommon/dobj_management.cpp under
// COD4X1KHZ && KISAK_MP. Actual DObj graph/model payload is restored jointly
// with native MT, XAnim trees/info and the dedicated server skeleton arena.
#include "khz_server_checkpoint_native.h"
#include <xanim/xanim.h>
#include <script/scr_stringlist.h>
extern scrMemTreePub_t scrMemTreePub;
namespace {
using namespace khz::server_native_checkpoint;
Bytes<sizeof(objBuf)> khzServerDObjBufferImage;
Bytes<sizeof(objAlloced)> khzServerDObjAllocationImage;
Bytes<sizeof(clientObjMap)> khzServerDObjClientMapImage;
Bytes<sizeof(serverObjMap)> khzServerDObjServerMapImage;
Bytes<sizeof(clientObjMapBuffered)> khzServerDObjBufferedMapImage;
Bytes<sizeof(serverObjDirty)> khzServerDObjDirtyImage;
KHZ_ServerCheckpointFrame khzServerDObjFrame;
int khzServerDObjFreeCount, khzServerDObjLastIndex, khzServerDObjInited;
bool khzServerDObjCaptured;
bool khzServerDObjState(char* text, unsigned capacity) {
    if (!context(text, capacity) || !KHZ_ServerScriptCheckpointIdle(text, capacity)) return false;
    if (!g_bDObjInited || objAlloced[0] || objFreeCount < 0 || objFreeCount >= DOBJ_HANDLE_MAX ||
        com_lastDObjIndex < 1 || com_lastDObjIndex >= DOBJ_HANDLE_MAX)
        return reject(text, capacity, "server_dobj_native_allocator_state_invalid");
    for (const auto value : clientObjMap) if (value) return reject(text, capacity, "server_dobj_dedicated_fixture_contains_client_graph");
    for (const auto value : clientObjMapBuffered) if (value) return reject(text, capacity, "server_dobj_dedicated_fixture_contains_buffered_client_graph");
    unsigned char referenced[DOBJ_HANDLE_MAX]{};
    for (const auto value : serverObjMap) if (value) {
        if (value < 0 || value >= DOBJ_HANDLE_MAX || !objAlloced[value] || referenced[value])
            return reject(text, capacity, "server_dobj_native_server_map_invalid");
        referenced[value] = 1;
    }
    unsigned used = 0;
    for (unsigned index = 1; index < DOBJ_HANDLE_MAX; ++index) if (objAlloced[index]) {
        ++used; const auto& obj = objBuf[index];
        if (!referenced[index] || obj.locked || !obj.numModels || obj.numModels > DOBJ_MAX_SUBMODELS ||
            !obj.numBones || obj.numBones > DOBJ_MAX_PARTS ||
            !KHZ_ServerCheckpointRestoresRange(KHZ_ServerCheckpointDomain::ScriptAllocators, obj.models, 5u * obj.numModels))
            return reject(text, capacity, "server_dobj_uncaptured_model_list_or_native_lock_invalid");
        if (obj.tree && !KHZ_ServerCheckpointRestoresAddress(obj.tree, sizeof(XAnimTree_s)))
            return reject(text, capacity, "server_dobj_native_animation_tree_has_no_restore_provider");
        if (obj.duplicateParts && !KHZ_ServerCheckpointRestoresRange(KHZ_ServerCheckpointDomain::ScriptAllocators,
            scrMemTreePub.mt_buffer + obj.duplicateParts * MT_NODE_SIZE, sizeof(std::uint32_t) + obj.duplicatePartsSize))
            return reject(text, capacity, "server_dobj_native_duplicate_parts_have_no_restore_provider");
        if (obj.skel.mat && !KHZ_ServerCheckpointRestoresAddress(obj.skel.mat, obj.numBones * sizeof(DObjAnimMat)))
            return reject(text, capacity, "server_dobj_native_skeleton_has_no_restore_provider");
    }
    return used + unsigned(objFreeCount) == DOBJ_HANDLE_MAX - 1 ? true :
        reject(text, capacity, "server_dobj_native_allocation_coverage_incomplete");
}
bool khzServerDObjCapture(const KHZ_ServerCheckpointFrame& f, char* text, unsigned capacity) {
    if (!khzServerDObjState(text, capacity)) return false;
    khzServerDObjBufferImage.capture(objBuf); khzServerDObjAllocationImage.capture(objAlloced);
    khzServerDObjClientMapImage.capture(clientObjMap); khzServerDObjServerMapImage.capture(serverObjMap);
    khzServerDObjBufferedMapImage.capture(clientObjMapBuffered); khzServerDObjDirtyImage.capture(serverObjDirty);
    khzServerDObjFreeCount = objFreeCount; khzServerDObjLastIndex = com_lastDObjIndex; khzServerDObjInited = g_bDObjInited;
    khzServerDObjFrame = f; khzServerDObjCaptured = true; return true;
}
bool khzServerDObjValidate(const KHZ_ServerCheckpointFrame& f, char* text, unsigned capacity) {
    if (!khzServerDObjCaptured || !khz::server_checkpoint::sameLifetime(khzServerDObjFrame, f))
        return reject(text, capacity, "server_dobj_image_lifetime_missing_or_changed");
    return khzServerDObjState(text, capacity);
}
void khzServerDObjRestore(const KHZ_ServerCheckpointFrame&) {
    iassert(KHZ_ServerCheckpointContext());
    khzServerDObjBufferImage.restore(objBuf); khzServerDObjAllocationImage.restore(objAlloced);
    khzServerDObjClientMapImage.restore(clientObjMap); khzServerDObjServerMapImage.restore(serverObjMap);
    khzServerDObjBufferedMapImage.restore(clientObjMapBuffered); khzServerDObjDirtyImage.restore(serverObjDirty);
    objFreeCount = khzServerDObjFreeCount; com_lastDObjIndex = khzServerDObjLastIndex; g_bDObjInited = khzServerDObjInited;
}
void khzServerDObjDiscard() { khzServerDObjCaptured = false; }
}
void KHZ_ServerDObjRegisterCheckpoint() {
    const KHZ_ServerCheckpointProvider p{KHZ_ServerCheckpointDomain::Graphs, 1,
        sizeof(khzServerDObjBufferImage) + sizeof(khzServerDObjAllocationImage) + sizeof(khzServerDObjClientMapImage) +
        sizeof(khzServerDObjServerMapImage) + sizeof(khzServerDObjBufferedMapImage) + sizeof(khzServerDObjDirtyImage),
        khzServerDObjCapture, khzServerDObjValidate, khzServerDObjRestore, khzServerDObjDiscard};
    if (!KHZ_ServerCheckpointRegister(p) ||
        !KHZ_ServerCheckpointRegisterRange(p.domain, objBuf, sizeof(objBuf)) ||
        !KHZ_ServerCheckpointRegisterRange(p.domain, objAlloced, sizeof(objAlloced)) ||
        !KHZ_ServerCheckpointRegisterRange(p.domain, serverObjMap, sizeof(serverObjMap)))
        Com_Error(ERR_FATAL, "Could not register native server DObj graph checkpoint");
}
