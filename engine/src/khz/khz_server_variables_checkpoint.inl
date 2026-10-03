// Source-owned seam: include at the END of script/scr_variable.cpp under
// COD4X1KHZ && KISAK_MP. This restores the complete native object/child graph,
// free-list ordering, IDs and refcount diagnostics at their original addresses.
#include "khz_server_checkpoint_native.h"
namespace {
using namespace khz::server_native_checkpoint;
Bytes<sizeof(scrVarPub)> khzServerVarPubImage;
Bytes<sizeof(scrVarGlob)> khzServerVarGlobImage;
Bytes<sizeof(scrVarDebugPubBuf)> khzServerVarDebugImage;
Bytes<sizeof(g_classMap)> khzServerVarClassImage;
KHZ_ServerCheckpointFrame khzServerVarFrame;
scrVarDebugPub_t* khzServerVarDebugAddress;
HunkUser* khzServerVarProgramHunk;
const char* khzServerVarProgramEnd;
char* khzServerVarFieldBuffer;
std::uint32_t khzServerVarChecksum;
std::uint16_t khzServerVarCanonicalStrings;
bool khzServerVarCaptured;
bool khzServerVarState(const KHZ_ServerCheckpointFrame& f, char* text, unsigned capacity) {
    if (!KHZ_ServerScriptCheckpointIdle(text, capacity)) return false;
    if (f.scriptProgramAddress != reinterpret_cast<std::uintptr_t>(scrVarPub.programBuffer) ||
        scrVarPub.endScriptBuffer < scrVarPub.programBuffer || !scrVarPub.programHunkUser ||
        (scrVarDebugPub && scrVarDebugPub != &scrVarDebugPubBuf))
        return reject(text, capacity, "server_variables_program_or_debug_arena_invalid");
    if (scrVarPub.numScriptThreads > VARIABLELIST_PARENT_SIZE || scrVarPub.numScriptObjects > VARIABLELIST_PARENT_SIZE ||
        scrVarPub.timeArrayId >= VARIABLELIST_PARENT_SIZE || scrVarPub.pauseArrayId >= VARIABLELIST_PARENT_SIZE ||
        scrVarPub.levelId >= VARIABLELIST_PARENT_SIZE || scrVarPub.gameId >= VARIABLELIST_PARENT_SIZE ||
        scrVarPub.animId >= VARIABLELIST_PARENT_SIZE)
        return reject(text, capacity, "server_variables_native_root_id_invalid");
    return true;
}
bool khzServerVarCapture(const KHZ_ServerCheckpointFrame& f, char* text, unsigned capacity) {
    if (!khzServerVarState(f, text, capacity)) return false;
    khzServerVarPubImage.capture(&scrVarPub); khzServerVarGlobImage.capture(&scrVarGlob);
    khzServerVarDebugImage.capture(&scrVarDebugPubBuf); khzServerVarClassImage.capture(g_classMap);
    khzServerVarDebugAddress = scrVarDebugPub; khzServerVarProgramHunk = scrVarPub.programHunkUser;
    khzServerVarProgramEnd = scrVarPub.endScriptBuffer; khzServerVarFieldBuffer = scrVarPub.fieldBuffer;
    khzServerVarChecksum = scrVarPub.checksum; khzServerVarCanonicalStrings = scrVarPub.canonicalStrCount;
    khzServerVarFrame = f; khzServerVarCaptured = true; return true;
}
bool khzServerVarValidate(const KHZ_ServerCheckpointFrame& f, char* text, unsigned capacity) {
    if (!khzServerVarCaptured || !khz::server_checkpoint::sameLifetime(khzServerVarFrame, f) ||
        scrVarDebugPub != khzServerVarDebugAddress || scrVarPub.programHunkUser != khzServerVarProgramHunk ||
        scrVarPub.endScriptBuffer != khzServerVarProgramEnd || scrVarPub.fieldBuffer != khzServerVarFieldBuffer ||
        scrVarPub.checksum != khzServerVarChecksum || scrVarPub.canonicalStrCount != khzServerVarCanonicalStrings)
        return reject(text, capacity, "server_variables_image_or_pinned_program_identity_changed");
    return khzServerVarState(f, text, capacity);
}
void khzServerVarRestore(const KHZ_ServerCheckpointFrame&) {
    iassert(KHZ_ServerCheckpointContext());
    khzServerVarPubImage.restore(&scrVarPub); khzServerVarGlobImage.restore(&scrVarGlob);
    khzServerVarDebugImage.restore(&scrVarDebugPubBuf); khzServerVarClassImage.restore(g_classMap);
    scrVarDebugPub = khzServerVarDebugAddress;
}
void khzServerVarDiscard() { khzServerVarCaptured = false; }
}
void KHZ_ServerScriptVariablesRegisterCheckpoint() {
    const KHZ_ServerCheckpointProvider p{KHZ_ServerCheckpointDomain::ScriptVM, 1,
        sizeof(khzServerVarPubImage) + sizeof(khzServerVarGlobImage) + sizeof(khzServerVarDebugImage) + sizeof(khzServerVarClassImage),
        khzServerVarCapture, khzServerVarValidate, khzServerVarRestore, khzServerVarDiscard};
    if (!KHZ_ServerCheckpointRegister(p) ||
        !KHZ_ServerCheckpointRegisterRange(p.domain, &scrVarPub, sizeof(scrVarPub)) ||
        !KHZ_ServerCheckpointRegisterRange(p.domain, &scrVarGlob, sizeof(scrVarGlob)) ||
        !KHZ_ServerCheckpointRegisterRange(p.domain, &scrVarDebugPubBuf, sizeof(scrVarDebugPubBuf)) ||
        !KHZ_ServerCheckpointRegisterRange(p.domain, g_classMap, sizeof(g_classMap)))
        Com_Error(ERR_FATAL, "Could not register native server script variable graph checkpoint");
}
