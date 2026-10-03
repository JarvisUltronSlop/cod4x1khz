// Include at END of khz_runtime.cpp. Native VM/allocator parts remain mandatory
// together with this heap/calendar; sleeping script buffers are never inferred
// from deadlines or rebuilt in a new order.
#include "khz_server_schedule_checkpoint.h"
#include "khz_server_checkpoint_native.h"
#include <universal/com_math.h>
#include <fp_environment.hpp>
#include <type_traits>

namespace {
using namespace khz::server_native_checkpoint;
struct ServerScheduleImage {
    KHZ_ServerCheckpointFrame frame;
    Bytes<sizeof(scriptCalendar)> calendar;
    khz::Tick now;
    bool frameEnd, captured;
};
ServerScheduleImage serverScheduleImage;
static_assert(std::is_trivially_copyable_v<decltype(scriptCalendar)>);
bool serverScheduleBoundary(const KHZ_ServerCheckpointFrame& f, char* text, unsigned capacity) {
    if (!context(text, capacity)) return false;
    if (session != f.session || serverTick != f.logicalTick || svs.time != f.nativeTime ||
        scriptFrameEnd)
        return reject(text, capacity, "server_script_calendar_not_at_completed_native_tick");
    return true;
}
bool serverScheduleCapture(const KHZ_ServerCheckpointFrame& f, char* text, unsigned capacity) {
    if (!serverScheduleBoundary(f, text, capacity)) return false;
    auto& image = serverScheduleImage; image.frame = f;
    image.calendar.capture(&scriptCalendar); image.now = scriptNow; image.frameEnd = scriptFrameEnd;
    image.captured = true; return true;
}
bool serverScheduleValidate(const KHZ_ServerCheckpointFrame& f, char* text, unsigned capacity) {
    if (!serverScheduleBoundary(f, text, capacity)) return false;
    const auto& image = serverScheduleImage;
    if (!image.captured || !khz::server_checkpoint::sameLifetime(f, image.frame) ||
        f.logicalTick < image.frame.logicalTick || scriptNow < image.now ||
        f.logicalTick - image.frame.logicalTick != scriptNow - image.now)
        return reject(text, capacity, "server_script_calendar_original_arena_fence_or_clock_phase_changed");
    return true;
}
void serverScheduleRestore(const KHZ_ServerCheckpointFrame&) {
    iassert(KHZ_ServerCheckpointContext());
    serverScheduleImage.calendar.restore(&scriptCalendar);
    scriptNow = serverScheduleImage.now; scriptFrameEnd = serverScheduleImage.frameEnd;
}
void serverScheduleDiscard() { serverScheduleImage.captured = false; }
struct ServerRandomImage {
    KHZ_ServerCheckpointFrame frame;
    khz::GameplayRandomState streams;
    unsigned x87, mxcsr;
    bool captured;
};
ServerRandomImage serverRandomImage;
bool serverRandomCapture(const KHZ_ServerCheckpointFrame& f, char* text, unsigned capacity) {
    if (!context(text, capacity)) return false;
    const auto fp = khz::gameplayFpEnvironment();
    if (!khz::isGameplayFpEnvironment(fp)) return reject(text, capacity, "server_rng_floating_point_environment_changed");
    serverRandomImage.frame = f; serverRandomImage.streams = KHZ_GetGameplayRandomState();
    serverRandomImage.x87 = fp.x87Control; serverRandomImage.mxcsr = fp.mxcsr;
    serverRandomImage.captured = true; return true;
}
bool serverRandomValidate(const KHZ_ServerCheckpointFrame& f, char* text, unsigned capacity) {
    if (!context(text, capacity)) return false;
    const auto fp = khz::gameplayFpEnvironment();
    if (!serverRandomImage.captured || !khz::server_checkpoint::sameLifetime(f, serverRandomImage.frame) ||
        !khz::isGameplayFpEnvironment(fp) || fp.x87Control != serverRandomImage.x87 || fp.mxcsr != serverRandomImage.mxcsr)
        return reject(text, capacity, "server_rng_original_owner_or_floating_point_environment_changed");
    return true;
}
void serverRandomRestore(const KHZ_ServerCheckpointFrame&) {
    iassert(KHZ_ServerCheckpointContext());
    KHZ_SetGameplayRandomState(serverRandomImage.streams);
}
void serverRandomDiscard() { serverRandomImage.captured = false; }
}
void KHZ_ServerScheduleRegisterCheckpoint() {
    const KHZ_ServerCheckpointProvider p{KHZ_ServerCheckpointDomain::Schedule, 0, sizeof(serverScheduleImage),
        serverScheduleCapture, serverScheduleValidate, serverScheduleRestore, serverScheduleDiscard};
    if (!KHZ_ServerCheckpointRegister(p)) Com_Error(ERR_FATAL, "Duplicate native script calendar checkpoint provider");
}
void KHZ_ServerRandomRegisterCheckpoint() {
    // ODE seed/world state belongs to the mandatory native Physics provider.
    const KHZ_ServerCheckpointProvider p{KHZ_ServerCheckpointDomain::RNG, 0, sizeof(serverRandomImage),
        serverRandomCapture, serverRandomValidate, serverRandomRestore, serverRandomDiscard};
    if (!KHZ_ServerCheckpointRegister(p)) Com_Error(ERR_FATAL, "Duplicate native gameplay RNG checkpoint provider");
}
