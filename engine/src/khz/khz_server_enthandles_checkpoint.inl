// Source-owned seam: include at the END of game/enthandle.cpp under
// COD4X1KHZ && KISAK_MP. Preserve handle lists/free-list order and raw references
// only when their actual owner storage is covered by another restore provider.
#include "khz_server_checkpoint_native.h"
namespace {
using namespace khz::server_native_checkpoint;
Bytes<sizeof(g_entitiesHandleList)> khzServerHandleListsImage;
Bytes<sizeof(g_entHandleInfoArray)> khzServerHandleInfoImage;
KHZ_ServerCheckpointFrame khzServerHandleFrame;
int khzServerHandleUsed, khzServerHandleHead, khzServerHandleMaximum;
bool khzServerHandlesCaptured;
bool khzServerHandleState(char* text, unsigned capacity) {
    if (!context(text, capacity) || !KHZ_ServerScriptCheckpointIdle(text, capacity)) return false;
    constexpr unsigned count = sizeof(g_entHandleInfoArray) / sizeof(g_entHandleInfoArray[0]);
    if (g_usedEntHandle < 0 || g_usedEntHandle >= int(count) || g_maxUsedEntHandle < g_usedEntHandle ||
        g_maxUsedEntHandle >= int(count) || g_entHandleInfoHead < 0 || g_entHandleInfoHead >= int(count))
        return reject(text, capacity, "server_enthandle_native_counters_invalid");
    unsigned char visited[count]{}; unsigned used = 0;
    for (unsigned entity = 0; entity < MAX_GENTITIES; ++entity) {
        const auto first = g_entitiesHandleList[entity].infoIndex;
        auto index = first;
        if (index >= count) return reject(text, capacity, "server_enthandle_entity_list_head_invalid");
        if (!index) continue;
        do {
            if (!index || index >= count || visited[index])
                return reject(text, capacity, "server_enthandle_entity_list_cycle_invalid");
            visited[index] = 1; ++used; const auto& info = g_entHandleInfoArray[index];
            if (!info.next || info.next >= count || !info.prev || info.prev >= count ||
                g_entHandleInfoArray[info.next].prev != index || g_entHandleInfoArray[info.prev].next != index ||
                !info.handle || reinterpret_cast<std::uintptr_t>(info.handle) % alignof(EntHandle) ||
                !KHZ_ServerCheckpointRestoresAddress(info.handle, sizeof(EntHandle)))
                return reject(text, capacity, "server_enthandle_link_or_uncaptured_reference_invalid");
            const auto* handle = static_cast<const EntHandle*>(info.handle);
            if (handle->infoIndex != index || handle->number != entity + 1)
                return reject(text, capacity, "server_enthandle_native_reference_disagrees_with_graph");
            index = info.next;
        } while (index != first);
    }
    if (used != unsigned(g_usedEntHandle)) return reject(text, capacity, "server_enthandle_used_count_disagrees_with_graph");
    auto index = unsigned(g_entHandleInfoHead); unsigned free = 0;
    while (index) {
        if (index >= count || visited[index] || g_entHandleInfoArray[index].handle)
            return reject(text, capacity, "server_enthandle_native_free_list_invalid");
        visited[index] = 1; ++free; index = g_entHandleInfoArray[index].next;
    }
    return used + free == count - 1 ? true : reject(text, capacity, "server_enthandle_native_slot_coverage_incomplete");
}
bool khzServerHandlesCapture(const KHZ_ServerCheckpointFrame& f, char* text, unsigned capacity) {
    if (!khzServerHandleState(text, capacity)) return false;
    khzServerHandleListsImage.capture(g_entitiesHandleList); khzServerHandleInfoImage.capture(g_entHandleInfoArray);
    khzServerHandleUsed = g_usedEntHandle; khzServerHandleHead = g_entHandleInfoHead; khzServerHandleMaximum = g_maxUsedEntHandle;
    khzServerHandleFrame = f; khzServerHandlesCaptured = true; return true;
}
bool khzServerHandlesValidate(const KHZ_ServerCheckpointFrame& f, char* text, unsigned capacity) {
    if (!khzServerHandlesCaptured || !khz::server_checkpoint::sameLifetime(khzServerHandleFrame, f))
        return reject(text, capacity, "server_enthandle_image_lifetime_missing_or_changed");
    return khzServerHandleState(text, capacity);
}
void khzServerHandlesRestore(const KHZ_ServerCheckpointFrame&) {
    iassert(KHZ_ServerCheckpointContext());
    khzServerHandleListsImage.restore(g_entitiesHandleList); khzServerHandleInfoImage.restore(g_entHandleInfoArray);
    g_usedEntHandle = khzServerHandleUsed; g_entHandleInfoHead = khzServerHandleHead; g_maxUsedEntHandle = khzServerHandleMaximum;
}
void khzServerHandlesDiscard() { khzServerHandlesCaptured = false; }
}
void KHZ_ServerEntHandlesRegisterCheckpoint() {
    const KHZ_ServerCheckpointProvider p{KHZ_ServerCheckpointDomain::Graphs, 0,
        sizeof(khzServerHandleListsImage) + sizeof(khzServerHandleInfoImage),
        khzServerHandlesCapture, khzServerHandlesValidate, khzServerHandlesRestore, khzServerHandlesDiscard};
    if (!KHZ_ServerCheckpointRegister(p) ||
        !KHZ_ServerCheckpointRegisterRange(p.domain, g_entitiesHandleList, sizeof(g_entitiesHandleList)) ||
        !KHZ_ServerCheckpointRegisterRange(p.domain, g_entHandleInfoArray, sizeof(g_entHandleInfoArray)))
        Com_Error(ERR_FATAL, "Could not register native server entity-handle graph checkpoint");
}
