#include "khz_server_metrics.h"
#include "khz_archive.h"
#include <core.hpp>
#include <qcommon/threads.h>
#include <script/scr_parser.h>
#include <script/scr_variable.h>
#include <script/scr_main.h>
#include <qcommon/qcommon.h>
#include <cstdio>
#include <cstring>

namespace {
constexpr unsigned Count = static_cast<unsigned>(KHZ_ServerPhase::Count);
constexpr const char* Names[Count] = {"preparation", "pre_frame", "input", "gameplay", "post_frame", "snapshots",
    "player_triggers", "animation_clock", "due_scripts", "player_notifies", "animation_update", "entities", "client_end_frame", "frame_end", "compression", "archive"};
struct Phase { khz::Histogram work; std::uint64_t totalQpc = 0; };
Phase phases[Count]; // BSS; no large temporary on the Win32 stack.
std::uint64_t current[Count], samples = 0, latestTick = 0;
bool active = false;
bool scriptProfiling = false;
struct ScriptSlice { const char* position = nullptr; std::uint64_t count = 0, total = 0, maximum = 0; };
ScriptSlice slices[1024];
std::uint64_t sliceOverflows = 0;
void escaped(const char* value, char* output, unsigned capacity) {
    unsigned used = 0;
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(value ? value : ""); *p && used + 2 < capacity; ++p) {
        if (*p == '"' || *p == '\\') output[used++] = '\\';
        output[used++] = *p < 32 || *p >= 127 ? ' ' : static_cast<char>(*p);
    }
    output[used] = 0;
}
}
std::uint64_t KHZ_ServerMetricsCounter() { return khz::counter(); }
bool KHZ_ServerMetricsActive() { return active && Sys_IsMainThread(); }
bool KHZ_ServerMetricsScriptActive() { return scriptProfiling && KHZ_ServerMetricsActive(); }
void KHZ_ServerMetricsReset() {
    if (!Sys_IsMainThread()) return;
    std::memset(phases, 0, sizeof(phases));
    std::memset(current, 0, sizeof(current)); samples = latestTick = 0; active = false;
    std::memset(slices, 0, sizeof(slices)); sliceOverflows = 0;
}
void KHZ_ServerMetricsBeginTick(std::uint64_t tick) {
    if (!Sys_IsMainThread()) return;
    std::memset(current, 0, sizeof(current)); latestTick = tick; active = true;
    scriptProfiling = Dvar_GetBool("khz_profile_scripts");
}
void KHZ_ServerMetricsRecord(KHZ_ServerPhase phase, std::uint64_t elapsed) {
    const auto index = static_cast<unsigned>(phase);
    if (KHZ_ServerMetricsActive() && index < Count) current[index] += elapsed;
}
void KHZ_ServerMetricsScriptSlice(const char* position, std::uint64_t elapsed) {
    if (!position || !KHZ_ServerMetricsActive()) return;
    const auto first = (reinterpret_cast<std::uintptr_t>(position) >> 2) & 1023;
    for (unsigned offset = 0; offset < 1024; ++offset) {
        auto& slice = slices[(first + offset) & 1023];
        if (!slice.position) slice.position = position;
        if (slice.position == position) {
            ++slice.count; slice.total += elapsed;
            if (slice.maximum < elapsed) slice.maximum = elapsed;
            return;
        }
    }
    ++sliceOverflows;
}
void KHZ_ServerMetricsEndTick() {
    if (!KHZ_ServerMetricsActive()) return;
    active = false; ++samples;
    const auto frequency = khz::frequency();
    for (unsigned i = 0; i < Count; ++i) {
        phases[i].totalQpc += current[i];
        phases[i].work.add(current[i] * 1000000 / frequency);
    }
}
const char* KHZ_ServerMetricsJSON() {
    if (!Sys_IsMainThread()) return "{}";
    static char result[8192];
    int used = std::snprintf(result, sizeof(result), "{\"samples\":%llu,\"latest_tick\":%llu,\"overlapping_phases\":true,\"phases\":{", samples, latestTick);
    for (unsigned i = 0; i < Count; ++i) {
        const auto& phase = phases[i];
        used += std::snprintf(result + used, sizeof(result) - used,
            "%s\"%s\":{\"count\":%llu,\"total_us\":%llu,\"p99_us\":%u,\"max_us\":%llu}",
            i ? "," : "", Names[i], phase.work.count, phase.totalQpc * 1000000 / khz::frequency(),
            phase.work.percentile(99), phase.work.maximum);
    }
    used += std::snprintf(result + used, sizeof(result) - used,
        "},\"script_slices\":{\"enabled\":%s,\"inclusive_nested\":true,\"overflow\":%llu,\"slowest\":[", scriptProfiling ? "true" : "false", sliceOverflows);
    const ScriptSlice* selected[8]{};
    for (const auto& slice : slices) if (slice.position) {
        for (unsigned i = 0; i < 8; ++i) if (!selected[i] || slice.maximum > selected[i]->maximum) {
            for (unsigned j = 7; j > i; --j) selected[j] = selected[j - 1];
            selected[i] = &slice; break;
        }
    }
    for (unsigned i = 0; i < 8 && selected[i]; ++i) {
        const auto& slice = *selected[i]; char file[256]{}, function[192]{};
        const bool sourceAvailable = scrVarPub.developer && scrParserPub.sourceBufferLookup &&
            scrParserPub.sourceBufferLookupLen && Scr_IsInOpcodeMemory(slice.position);
        if (sourceAvailable) {
            escaped(Scr_PrevCodePosFileName(const_cast<char*>(slice.position)), file, sizeof(file));
            escaped(Scr_PrevCodePosFunctionName(const_cast<char*>(slice.position)), function, sizeof(function));
        }
        used += std::snprintf(result + used, sizeof(result) - used,
            "%s{\"code_offset\":%llu,\"count\":%llu,\"total_us\":%llu,\"maximum_us\":%llu,\"source_available\":%s,\"file\":\"%s\",\"function\":\"%s\"}",
            i ? "," : "", static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(slice.position) -
                reinterpret_cast<std::uintptr_t>(scrVarPub.programBuffer)), slice.count,
            slice.total * 1000000 / khz::frequency(), slice.maximum * 1000000 / khz::frequency(),
            sourceAvailable ? "true" : "false", file, function);
    }
    std::snprintf(result + used, sizeof(result) - used, "]},\"archive\":%s}", KHZ_ArchiveDiagnosticsJSON()); return result;
}
