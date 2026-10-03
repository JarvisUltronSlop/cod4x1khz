// Include at END of universal/dvar.cpp under COD4X1KHZ && KISAK_MP. This must
// accompany the pre-mutation guards in the native Dvar_* entry points. No
// string allocation/free or registration may bypass those guards while pinned.
#include "khz_server_dvars_checkpoint.h"
#include "khz_server_checkpoint.h"
#include <cstdint>
#include <cstring>
#include <cstdio>

namespace {
struct DvarsCheckpointImage {
    KHZ_ServerCheckpointFrame frame;
    decltype(dvarPool) pool;
    decltype(dvarHashTable) hashes;
    decltype(sortedDvars) sorted;
    std::uint64_t immutable[4096];
    const dvar_s* cheats;
    int count, modifiedFlags;
    LONG sortedFlag;
    bool sortedOrder, active, autoExec, captured;
};
DvarsCheckpointImage dvarsCheckpointImage; // Fixed BSS, never a stack image.
bool dvarsCheckpointReject(char* reason, unsigned capacity, const char* text) {
    if (reason && capacity) std::snprintf(reason, capacity, "%s", text);
    return false;
}
bool dvarsCheckpointText(std::uint64_t& hash, const char* text) {
    if (!text) { hash = (hash ^ 0xffu) * 1099511628211ull; return true; }
    // Native info/pure strings can exceed MAX_STRING_CHARS. A bounded scan is
    // enough to attest retained native strings without inventing ownership.
    for (unsigned i = 0; i != 65536; ++i) {
        const unsigned char ch = static_cast<unsigned char>(text[i]);
        hash = (hash ^ ch) * 1099511628211ull;
        if (!ch) return true;
    }
    return false;
}
bool dvarsCheckpointImmutable(const dvar_s& node, std::uint64_t* output) {
    std::uint64_t hash = 14695981039346656037ull;
    if (!node.name || !dvarsCheckpointText(hash, node.name) || !dvarsCheckpointText(hash, node.description)) return false;
    if (node.type == DVAR_TYPE_STRING) {
        if (!dvarsCheckpointText(hash, node.current.string) || !dvarsCheckpointText(hash, node.latched.string) ||
            !dvarsCheckpointText(hash, node.reset.string)) return false;
    } else if (node.type == DVAR_TYPE_ENUM) {
        if (node.domain.enumeration.stringCount < 0 || node.domain.enumeration.stringCount > 4096 ||
            (node.domain.enumeration.stringCount && !node.domain.enumeration.strings)) return false;
        for (int i = 0; i != node.domain.enumeration.stringCount; ++i)
            if (!dvarsCheckpointText(hash, node.domain.enumeration.strings[i])) return false;
    }
    *output = hash; return true;
}
bool dvarsCheckpointBoundary(char* reason, unsigned capacity) {
    if (!KHZ_ServerCheckpointContext() || !isDvarSystemActive || dvarCount < 1 || dvarCount > 4096 ||
        g_dvarCritSect.readCount || g_dvarCritSect.writeCount || isSortingDvars)
        return dvarsCheckpointReject(reason, capacity, "dvars_checkpoint_requires_idle_original_native_pool");
    return true;
}
bool dvarsCheckpointCapture(const KHZ_ServerCheckpointFrame& frame, char* reason, unsigned capacity) {
    if (!dvarsCheckpointBoundary(reason, capacity)) return false;
    for (int i = 0; i != dvarCount; ++i)
        if (!dvarsCheckpointImmutable(dvarPool[i], &dvarsCheckpointImage.immutable[i]))
            return dvarsCheckpointReject(reason, capacity, "dvars_checkpoint_immutable_string_or_domain_is_unbounded");
    auto& image = dvarsCheckpointImage;
    image.frame = frame;
    std::memcpy(image.pool, dvarPool, sizeof(dvarPool));
    std::memcpy(image.hashes, dvarHashTable, sizeof(dvarHashTable));
    std::memcpy(image.sorted, sortedDvars, sizeof(sortedDvars));
    image.cheats = dvar_cheats; image.count = dvarCount; image.modifiedFlags = dvar_modifiedFlags;
    image.sortedFlag = isSortedDvars; image.sortedOrder = areDvarsSorted;
    image.active = isDvarSystemActive; image.autoExec = isLoadingAutoExecGlobalFlag;
    image.captured = true; return true;
}
bool dvarsCheckpointValidate(const KHZ_ServerCheckpointFrame& frame, char* reason, unsigned capacity) {
    if (!dvarsCheckpointBoundary(reason, capacity)) return false;
    const auto& image = dvarsCheckpointImage;
    if (!image.captured || !khz::server_checkpoint::sameLifetime(frame, image.frame) || image.count != dvarCount ||
        image.cheats != dvar_cheats || std::memcmp(image.hashes, dvarHashTable, sizeof(dvarHashTable)))
        return dvarsCheckpointReject(reason, capacity, "dvars_checkpoint_native_registry_identity_changed");
    for (int i = 0; i != dvarCount; ++i) {
        const auto& live = dvarPool[i]; const auto& saved = image.pool[i];
        std::uint64_t immutable = 0;
        if (live.name != saved.name || live.description != saved.description || live.type != saved.type ||
            live.hashNext != saved.hashNext || live.domainFunc != saved.domainFunc ||
            std::memcmp(&live.domain, &saved.domain, sizeof(live.domain)) ||
            (live.type == DVAR_TYPE_STRING && (live.current.string != saved.current.string ||
             live.latched.string != saved.latched.string || live.reset.string != saved.reset.string)) ||
            !dvarsCheckpointImmutable(live, &immutable) || immutable != image.immutable[i])
            return dvarsCheckpointReject(reason, capacity, "dvars_checkpoint_unrestored_string_or_domain_lifetime_changed");
    }
    return true;
}
void dvarsCheckpointRestore(const KHZ_ServerCheckpointFrame&) {
    iassert(KHZ_ServerCheckpointContext());
    const auto& image = dvarsCheckpointImage;
    std::memcpy(dvarPool, image.pool, sizeof(dvarPool));
    std::memcpy(dvarHashTable, image.hashes, sizeof(dvarHashTable));
    std::memcpy(sortedDvars, image.sorted, sizeof(sortedDvars));
    dvarCount = image.count; dvar_modifiedFlags = image.modifiedFlags; dvar_cheats = image.cheats;
    isSortedDvars = image.sortedFlag; areDvarsSorted = image.sortedOrder;
    isDvarSystemActive = image.active; isLoadingAutoExecGlobalFlag = image.autoExec;
}
void dvarsCheckpointDiscard() { dvarsCheckpointImage.captured = false; }
}
bool KHZ_ServerDvarsAllowValueMutation(const dvar_s* node, DvarValue value) {
    if (!KHZ_ServerCheckpointPinned() || !node || node->type != DVAR_TYPE_STRING) return true;
    // Even equal text may free a distinct reset/latched allocation. Only an
    // already shared three-way alias with equal text can remain a no-op.
    if (node->current.string && value.string && node->current.string == node->latched.string &&
        node->current.string == node->reset.string && !std::strcmp(node->current.string, value.string)) return true;
    KHZ_ServerCheckpointRejectExternalEffect("dvar_string_mutation_requires_native_heap_rewind");
    return false;
}
bool KHZ_ServerDvarsAllowTopologyMutation() {
    if (!KHZ_ServerCheckpointPinned()) return true;
    KHZ_ServerCheckpointRejectExternalEffect("dvar_registry_mutation_requires_native_heap_rewind");
    return false;
}
void KHZ_ServerDvarsRegisterCheckpoint() {
    if (!KHZ_ServerCheckpointRegisterRange(KHZ_ServerCheckpointDomain::Dvars, dvarPool, sizeof(dvarPool)))
        Com_Error(ERR_FATAL, "Native Dvar checkpoint range could not register");
    const KHZ_ServerCheckpointProvider provider{KHZ_ServerCheckpointDomain::Dvars, 0, sizeof(dvarsCheckpointImage),
        dvarsCheckpointCapture, dvarsCheckpointValidate, dvarsCheckpointRestore, dvarsCheckpointDiscard};
    if (!KHZ_ServerCheckpointRegister(provider)) Com_Error(ERR_FATAL, "Duplicate native Dvar checkpoint provider");
}
