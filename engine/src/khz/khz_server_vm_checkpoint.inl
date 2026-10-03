// Source-owned seam: include at the END of script/scr_vm.cpp under
// COD4X1KHZ && KISAK_MP. No saved jmp_buf or active C++/native script stack is
// restored: an idle completed-tick VM is a real precondition for every provider.
#include "khz_server_checkpoint_native.h"
namespace {
using namespace khz::server_native_checkpoint;
Bytes<sizeof(scrVmPub)> khzServerVMPubImage;
Bytes<sizeof(scrVmGlob)> khzServerVMGlobImage;
Bytes<sizeof(scrVmDebugPub)> khzServerVMDebugImage;
Bytes<sizeof(fs)> khzServerVMFSImage;
Bytes<sizeof(error_message)> khzServerVMErrorImage;
KHZ_ServerCheckpointFrame khzServerVMFrame;
int khzServerVMOpcode, khzServerVMCaseCount, khzServerVMThreadCount;
bool khzServerVMCaptured;
}
bool KHZ_ServerScriptCheckpointIdle(char* text, unsigned capacity) {
    using namespace khz::server_native_checkpoint;
    if (!context(text, capacity)) return false;
    if (!scrVarPub.bInited || !scrVarPub.programBuffer || !scrVarPub.endScriptBuffer ||
        scrVmPub.function_count || scrVmPub.inparamcount || scrVmPub.outparamcount ||
        scrVmPub.breakpointOutparamcount || scrVmPub.terminal_error || scrVarPub.evaluate ||
        scrVarPub.error_message || scrVarPub.error_index || scrVmGlob.loading || g_script_error_level != -1 ||
        scrVmPub.top != scrVmPub.stack || scrVmPub.maxstack != &scrVmPub.stack[2047] ||
        scrVmPub.function_frame != scrVmPub.function_frame_start ||
        scrVmPub.localVars != reinterpret_cast<std::uint32_t*>(&scrVmGlob.starttime))
        return reject(text, capacity, "server_vm_not_native_idle_completed_tick");
    if (scrDebuggerGlob.debugger_inited || scrDebuggerGlob.debugger_inited_main ||
        scrDebuggerGlob.debugger_inited_system || scrDebuggerGlob.atBreakpoint ||
        scrDebuggerGlob.run_debugger || scrDebuggerGlob.breakpoints || scrDebuggerGlob.variableBreakpoints ||
        scrDebuggerGlob.breakpointCount || scrVmPub.debugCode)
        return reject(text, capacity, "server_vm_debugger_state_requires_separate_provider");
    return true;
}
namespace {
bool khzServerVMCapture(const KHZ_ServerCheckpointFrame& f, char* text, unsigned capacity) {
    if (!KHZ_ServerScriptCheckpointIdle(text, capacity) ||
        f.scriptProgramAddress != reinterpret_cast<std::uintptr_t>(scrVarPub.programBuffer))
        return reject(text, capacity, "server_vm_original_program_or_idle_state_invalid");
    khzServerVMPubImage.capture(&scrVmPub); khzServerVMGlobImage.capture(&scrVmGlob);
    khzServerVMDebugImage.capture(&scrVmDebugPub); khzServerVMFSImage.capture(&fs);
    khzServerVMErrorImage.capture(error_message);
    khzServerVMOpcode = opcode; khzServerVMCaseCount = caseCount; khzServerVMThreadCount = thread_count;
    khzServerVMFrame = f; khzServerVMCaptured = true; return true;
}
bool khzServerVMValidate(const KHZ_ServerCheckpointFrame& f, char* text, unsigned capacity) {
    if (!khzServerVMCaptured || !khz::server_checkpoint::sameLifetime(khzServerVMFrame, f))
        return reject(text, capacity, "server_vm_image_lifetime_missing_or_changed");
    return KHZ_ServerScriptCheckpointIdle(text, capacity);
}
void khzServerVMRestore(const KHZ_ServerCheckpointFrame&) {
    iassert(KHZ_ServerCheckpointContext());
    khzServerVMPubImage.restore(&scrVmPub); khzServerVMGlobImage.restore(&scrVmGlob);
    khzServerVMDebugImage.restore(&scrVmDebugPub); khzServerVMFSImage.restore(&fs);
    khzServerVMErrorImage.restore(error_message);
    opcode = khzServerVMOpcode; caseCount = khzServerVMCaseCount; thread_count = khzServerVMThreadCount;
    // g_script_error remains untouched: every capture/validation proved level
    // -1, and future native VM_Execute establishes fresh stack-local setjmp.
}
void khzServerVMDiscard() { khzServerVMCaptured = false; }
}
void KHZ_ServerScriptVMRegisterCheckpoint() {
    const KHZ_ServerCheckpointProvider p{KHZ_ServerCheckpointDomain::ScriptVM, 0,
        sizeof(khzServerVMPubImage) + sizeof(khzServerVMGlobImage) + sizeof(khzServerVMDebugImage) +
        sizeof(khzServerVMFSImage) + sizeof(khzServerVMErrorImage),
        khzServerVMCapture, khzServerVMValidate, khzServerVMRestore, khzServerVMDiscard};
    if (!KHZ_ServerCheckpointRegister(p) ||
        !KHZ_ServerCheckpointRegisterRange(p.domain, &scrVmPub, sizeof(scrVmPub)) ||
        !KHZ_ServerCheckpointRegisterRange(p.domain, &scrVmGlob, sizeof(scrVmGlob)) ||
        !KHZ_ServerCheckpointRegisterRange(p.domain, &scrVmDebugPub, sizeof(scrVmDebugPub)) ||
        !KHZ_ServerCheckpointRegisterRange(p.domain, &fs, sizeof(fs)))
        Com_Error(ERR_FATAL, "Could not register native server script VM checkpoint");
}
