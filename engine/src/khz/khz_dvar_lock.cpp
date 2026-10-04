#include <universal/q_shared.h>
#include "khz_controls.h"
#include <Windows.h>
#include <ticket_mutex.hpp>

namespace {
khz::TicketMutex dvars;
SRWLOCK registryLock = SRWLOCK_INIT;
thread_local unsigned lookupDepth = 0, registryDepth = 0;
thread_local bool lookupLocked = false;
thread_local unsigned dvarDepth = 0;
}
void KHZ_DvarEnter() { if (!dvarDepth++) dvars.lock(); }
void KHZ_DvarLeave() { iassert(dvarDepth); if (!--dvarDepth) dvars.unlock(); }
void KHZ_DvarLookupEnter() {
    if (lookupDepth++) return;
    lookupLocked = !registryDepth;
    if (lookupLocked) AcquireSRWLockShared(&registryLock);
}
void KHZ_DvarLookupLeave() {
    iassert(lookupDepth);
    if (!--lookupDepth && lookupLocked) { ReleaseSRWLockShared(&registryLock); lookupLocked = false; }
}
void KHZ_DvarRegistryEnter() {
    iassert(!lookupDepth || registryDepth);
    if (!registryDepth++) AcquireSRWLockExclusive(&registryLock);
}
void KHZ_DvarRegistryLeave() {
    iassert(registryDepth);
    if (!--registryDepth) ReleaseSRWLockExclusive(&registryLock);
}
void KHZ_DvarLocksErrorUnwind() {
    while (lookupDepth) KHZ_DvarLookupLeave();
    while (registryDepth) KHZ_DvarRegistryLeave();
    while (dvarDepth) KHZ_DvarLeave();
}
unsigned KHZ_DvarLockDepth() { return dvarDepth; }
