// Source-owned: END universal/com_memory.cpp, opt-in dedicated only.
#include "khz_server_checkpoint_owner.h"
#include "khz_server_checkpoint_native.h"
#include "khz_host.h"
#include <server_mp/server_mp.h>

bool KHZ_ServerCheckpointObserveHunk(KHZ_ServerCheckpointFrame* frame, char* text, unsigned capacity) {
    using namespace khz::server_native_checkpoint;
    if (!Sys_IsMainThread() || !frame || !KHZ_ServerCheckpointPaused() || com_inServerFrame ||
        !s_hunkData || s_hunkTotal <= 0 || g_largeLocalPos ||
        hunk_low.permanent < 0 || hunk_high.permanent < 0 ||
        hunk_low.temp != hunk_low.permanent || hunk_high.temp != hunk_high.permanent ||
        hunk_low.permanent > s_hunkTotal || hunk_high.permanent > s_hunkTotal - hunk_low.permanent)
        return reject(text, capacity, "server_checkpoint_original_hunk_or_temporary_scratch_not_idle");
    // These are lifetime checks. Exact provider-owned spans, rather than the
    // entire surrounding hunk, determine which pointer targets are restored.
    frame->hunkBaseAddress = reinterpret_cast<std::uintptr_t>(s_hunkData);
    frame->hunkLowUsed = static_cast<std::size_t>(hunk_low.permanent);
    frame->hunkHighUsed = static_cast<std::size_t>(hunk_high.permanent);
    return true;
}
bool KHZ_ServerCheckpointAllowHunkLifetimeChange(const char* operation) {
    if (!Sys_IsMainThread()) return false;
    if (!KHZ_ServerCheckpointPinned()) return true;
    KHZ_ServerCheckpointRejectExternalEffect(operation ? operation : "server_checkpoint_hunk_lifetime_changed");
    return false;
}
