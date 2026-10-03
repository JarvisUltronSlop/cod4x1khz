// Source-owned seam: include at the END of script/scr_memorytree.cpp under
// COD4X1KHZ && KISAK_MP. Captures the actual allocator, including sleeping VM
// stacks, vectors, strings, tagInfo and packed DObj model/map allocations.
#include "khz_server_checkpoint_native.h"
namespace {
using namespace khz::server_native_checkpoint;
Bytes<sizeof(scrMemTreeGlob)> khzServerMTGlobImage;
Bytes<sizeof(scrMemTreePub)> khzServerMTPubImage;
Bytes<sizeof(scrMemTreeDebugGlob)> khzServerMTDebugImage;
KHZ_ServerCheckpointFrame khzServerMTFrame;
bool khzServerMTCaptured;
bool khzServerMTState(const KHZ_ServerCheckpointFrame& f, char* text, unsigned capacity) {
    if (!context(text, capacity) || !KHZ_ServerScriptCheckpointIdle(text, capacity)) return false;
    if (scrMemTreePub.mt_buffer != reinterpret_cast<char*>(scrMemTreeGlob.nodes) ||
        f.mtBufferAddress != reinterpret_cast<std::uintptr_t>(scrMemTreePub.mt_buffer))
        return reject(text, capacity, "server_mt_original_native_nodes_changed");
    if (scrMemTreeGlob.totalAlloc < 0 || scrMemTreeGlob.totalAlloc > MEMORY_NODE_COUNT ||
        scrMemTreeGlob.totalAllocBuckets < 0 || scrMemTreeGlob.totalAllocBuckets > MEMORY_NODE_COUNT)
        return reject(text, capacity, "server_mt_native_allocation_counters_invalid");
    return true;
}
bool khzServerMTCapture(const KHZ_ServerCheckpointFrame& f, char* text, unsigned capacity) {
    if (!khzServerMTState(f, text, capacity)) return false;
    khzServerMTGlobImage.capture(&scrMemTreeGlob); khzServerMTPubImage.capture(&scrMemTreePub);
    khzServerMTDebugImage.capture(&scrMemTreeDebugGlob); khzServerMTFrame = f; khzServerMTCaptured = true; return true;
}
bool khzServerMTValidate(const KHZ_ServerCheckpointFrame& f, char* text, unsigned capacity) {
    if (!khzServerMTCaptured || !khz::server_checkpoint::sameLifetime(khzServerMTFrame, f))
        return reject(text, capacity, "server_mt_image_lifetime_missing_or_changed");
    return khzServerMTState(f, text, capacity);
}
void khzServerMTRestore(const KHZ_ServerCheckpointFrame&) {
    iassert(KHZ_ServerCheckpointContext());
    khzServerMTGlobImage.restore(&scrMemTreeGlob); khzServerMTPubImage.restore(&scrMemTreePub);
    khzServerMTDebugImage.restore(&scrMemTreeDebugGlob);
}
void khzServerMTDiscard() { khzServerMTCaptured = false; }
}
void KHZ_ServerMTRegisterCheckpoint() {
    const KHZ_ServerCheckpointProvider p{KHZ_ServerCheckpointDomain::ScriptAllocators, 0,
        sizeof(khzServerMTGlobImage) + sizeof(khzServerMTPubImage) + sizeof(khzServerMTDebugImage),
        khzServerMTCapture, khzServerMTValidate, khzServerMTRestore, khzServerMTDiscard};
    if (!KHZ_ServerCheckpointRegister(p) ||
        !KHZ_ServerCheckpointRegisterRange(p.domain, &scrMemTreeGlob, sizeof(scrMemTreeGlob)) ||
        !KHZ_ServerCheckpointRegisterRange(p.domain, &scrMemTreePub, sizeof(scrMemTreePub)) ||
        !KHZ_ServerCheckpointRegisterRange(p.domain, &scrMemTreeDebugGlob, sizeof(scrMemTreeDebugGlob)))
        Com_Error(ERR_FATAL, "Could not register native server MT checkpoint");
}
