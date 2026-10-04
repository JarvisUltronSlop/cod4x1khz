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

namespace { thread_local unsigned controlDepth = 0; }
void KHZ_ControlsErrorUnwind() {
    KHZ_DvarLocksErrorUnwind();
    controlDepth = 0;
    if (Sys_IsMainThread() && KHZ_ClientOwnerControlsActive()) KHZ_ClientOwnerEndControls();
}
void KHZ_CommandControlBegin() {
    iassert(Sys_IsMainThread());
    if (controlDepth++) return;
    if (!KHZ_ClientOwnerRunning() || KHZ_ClientOwnerControlsActive()) return;
    if (!KHZ_ClientOwnerPark())
        Com_Error(ERR_DROP, "Gameplay command barrier park failed: %s dvar_depth=%u", KHZ_ClientOwnerBarrierFailure(), KHZ_DvarLockDepth());
    if (!KHZ_ClientOwnerBeginControls())
        Com_Error(ERR_DROP, "Gameplay command barrier control access failed: %s dvar_depth=%u", KHZ_ClientOwnerBarrierFailure(), KHZ_DvarLockDepth());
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
