// Source-owned seam: include at the END of script/scr_stringlist.cpp under
// COD4X1KHZ && KISAK_MP. RefString/RefVector payload and refcounts belong to the
// mandatory MT provider; this restores their actual SL hash/free-list identity.
#include "khz_server_checkpoint_native.h"
namespace {
using namespace khz::server_native_checkpoint;
Bytes<sizeof(scrStringGlob)> khzServerSLGlobImage;
Bytes<sizeof(scrStringDebugGlobBuf)> khzServerSLDebugImage;
KHZ_ServerCheckpointFrame khzServerSLFrame;
scrStringDebugGlob_t* khzServerSLDebugAddress;
bool khzServerSLCaptured;
bool khzServerSLState(char* text, unsigned capacity) {
    if (!context(text, capacity) || !KHZ_ServerScriptCheckpointIdle(text, capacity)) return false;
    if (!scrStringGlob.inited || (scrStringDebugGlob && scrStringDebugGlob != &scrStringDebugGlobBuf))
        return reject(text, capacity, "server_sl_native_table_or_debug_arena_invalid");
    if (scrStringGlob.nextFreeEntry && !inside(scrStringGlob.nextFreeEntry, scrStringGlob.hashTable,
        sizeof(scrStringGlob.hashTable), sizeof(HashEntry)))
        return reject(text, capacity, "server_sl_native_free_entry_pointer_invalid");
    for (unsigned i = 0; i < STRINGLIST_SIZE; ++i) {
        const auto& entry = scrStringGlob.hashTable[i];
        const auto status = entry.status_next & HASH_STAT_MASK;
        if ((entry.status_next & 0xffffu) >= STRINGLIST_SIZE ||
            (status != HASH_STAT_FREE && status != HASH_STAT_HEAD && status != HASH_STAT_MOVABLE))
            return reject(text, capacity, "server_sl_native_hash_link_invalid");
        if (status != HASH_STAT_FREE && (entry.u.str == 0 || entry.u.str >= MEMORY_NODE_COUNT))
            return reject(text, capacity, "server_sl_native_string_index_invalid");
    }
    return true;
}
bool khzServerSLCapture(const KHZ_ServerCheckpointFrame& f, char* text, unsigned capacity) {
    if (!khzServerSLState(text, capacity)) return false;
    khzServerSLGlobImage.capture(&scrStringGlob); khzServerSLDebugImage.capture(&scrStringDebugGlobBuf);
    khzServerSLDebugAddress = scrStringDebugGlob; khzServerSLFrame = f; khzServerSLCaptured = true; return true;
}
bool khzServerSLValidate(const KHZ_ServerCheckpointFrame& f, char* text, unsigned capacity) {
    if (!khzServerSLCaptured || !khz::server_checkpoint::sameLifetime(khzServerSLFrame, f) ||
        scrStringDebugGlob != khzServerSLDebugAddress)
        return reject(text, capacity, "server_sl_image_or_native_debug_identity_changed");
    return khzServerSLState(text, capacity);
}
void khzServerSLRestore(const KHZ_ServerCheckpointFrame&) {
    iassert(KHZ_ServerCheckpointContext());
    khzServerSLGlobImage.restore(&scrStringGlob); khzServerSLDebugImage.restore(&scrStringDebugGlobBuf);
    scrStringDebugGlob = khzServerSLDebugAddress;
}
void khzServerSLDiscard() { khzServerSLCaptured = false; }
}
void KHZ_ServerSLRegisterCheckpoint() {
    const KHZ_ServerCheckpointProvider p{KHZ_ServerCheckpointDomain::ScriptAllocators, 1,
        sizeof(khzServerSLGlobImage) + sizeof(khzServerSLDebugImage),
        khzServerSLCapture, khzServerSLValidate, khzServerSLRestore, khzServerSLDiscard};
    if (!KHZ_ServerCheckpointRegister(p) ||
        !KHZ_ServerCheckpointRegisterRange(p.domain, &scrStringGlob, sizeof(scrStringGlob)) ||
        !KHZ_ServerCheckpointRegisterRange(p.domain, &scrStringDebugGlobBuf, sizeof(scrStringDebugGlobBuf)))
        Com_Error(ERR_FATAL, "Could not register native server SL checkpoint");
}
