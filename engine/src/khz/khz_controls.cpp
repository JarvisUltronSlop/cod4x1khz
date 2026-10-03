#include <universal/q_shared.h>
#include "khz_controls.h"
#include "khz_client_owner.h"
#include "khz_client_input.h"
#include "khz_input_owner.h"
#include <client_mp/client_mp.h>
#include <client/client.h>
#include <qcommon/qcommon.h>
#include <qcommon/threads.h>
#include <ui/keycodes.h>
#include <Windows.h>

namespace {
INIT_ONCE initialized = INIT_ONCE_STATIC_INIT;
CRITICAL_SECTION dvars;
thread_local unsigned dvarDepth = 0, controlDepth = 0;
BOOL CALLBACK initialize(PINIT_ONCE, PVOID, PVOID*) { InitializeCriticalSection(&dvars); return TRUE; }
}
void KHZ_DvarEnter() { InitOnceExecuteOnce(&initialized, initialize, nullptr, nullptr); EnterCriticalSection(&dvars); ++dvarDepth; }
void KHZ_DvarLeave() { iassert(dvarDepth); --dvarDepth; LeaveCriticalSection(&dvars); }
void KHZ_ControlsErrorUnwind() {
    while (dvarDepth) KHZ_DvarLeave();
    controlDepth = 0;
    if (Sys_IsMainThread() && KHZ_ClientOwnerControlsActive()) KHZ_ClientOwnerEndControls();
}
void KHZ_CommandControlBegin() {
    iassert(Sys_IsMainThread());
    if (controlDepth++) return;
    if (!KHZ_ClientOwnerRunning() || KHZ_ClientOwnerControlsActive()) return;
    if (!KHZ_ClientOwnerPark())
        Com_Error(ERR_DROP, "Gameplay command barrier park failed: %s dvar_depth=%u", KHZ_ClientOwnerBarrierFailure(), dvarDepth);
    if (!KHZ_ClientOwnerBeginControls())
        Com_Error(ERR_DROP, "Gameplay command barrier control access failed: %s dvar_depth=%u", KHZ_ClientOwnerBarrierFailure(), dvarDepth);
}
void KHZ_CommandControlEnd() {
    iassert(Sys_IsMainThread());
    if (!controlDepth || --controlDepth) return;
    if (!KHZ_ClientOwnerRunning()) return;
    if (KHZ_ClientOwnerControlsActive()) KHZ_ClientOwnerEndControls();
    if (KHZ_ClientOwnerNativeBarrierPending()) return;
    if (!KHZ_ClientInputFreezeBindings()) Com_Error(ERR_DROP, "Gameplay binding exceeds bounded input storage");
    const bool gameplay = KHZ_ClientGameplayInputAllowed();
    KHZ_InputOwnerSetGameplay(gameplay, KHZ_ClientOwnerGeneration());
    KHZ_InputOwnerResume();
    if (!KHZ_ClientOwnerResumeControls()) Com_Error(ERR_DROP, "Gameplay command barrier did not resume");
}
