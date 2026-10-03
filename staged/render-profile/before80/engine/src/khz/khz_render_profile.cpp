#include <universal/q_shared.h>
#include "khz_render_profile.h"
#include "khz_paths.h"
#include "core.hpp"
#include <qcommon/threads.h>
#include <qcommon/cmd.h>
#include <d3d9.h>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace {
enum Phase : unsigned { Idle, Preparing, Recording, Draining, Complete, Aborted };
constexpr unsigned CpuCapacity = 524288, GpuCapacity = 32768, QueryCapacity = 64;
const char* stageNames[] = {"frontend", "consume", "worker_wait", "frontend_workers", "renderer_wait", "backend_idle", "present",
    "gpu_sync", "begin_frame", "cgame", "render_scene", "packet_entities", "view_weapon", "draw_2d", "draw_active",
    "vertex_lock", "index_lock", "world_dpvs", "bsp", "static_models", "dyn_models", "scene_entities", "model_lighting", "merge_surfs", "sound", "fx_physics", "dyn_entity_physics", "frame_work", "client_frame", "end_frame"};
struct CpuEvent { std::uint64_t begin, end; unsigned stage, thread; };
struct CpuBuffer {
    std::array<CpuEvent, CpuCapacity> events;
    unsigned written = 0;
    std::atomic<unsigned> published{0}, overflow{0};
};
struct GpuEvent { std::uint64_t begin = 0, end = 0, submitBegin = 0, submitEnd = 0, firstReady = 0; };
struct Slot { IDirect3DQuery9* begin = nullptr; IDirect3DQuery9* end = nullptr; int record = -1; };
CpuBuffer cpu[2]; // One writer each: actual main and actual backend.
std::array<GpuEvent, GpuCapacity> frames;
std::array<Slot, QueryCapacity> slots;
IDirect3DQuery9 *frequencyQuery = nullptr, *disjointQuery = nullptr;
std::atomic<unsigned> phase{Idle}, durationMs{0}, gpuPublished{0}, gpuReady{0}, gpuSkipped{0}, queryErrors{0};
std::atomic<std::uint64_t> epoch{0}, started{0}, ended{0}, gpuFrequency{0};
std::atomic<bool> supported{false}, disjoint{true};
unsigned frameCount = 0, readyCount = 0;
std::uint64_t deadline = 0, drainDeadline = 0;
int currentSlot = -1;
bool frequencyReady = false, disjointReady = false;
std::filesystem::path exported;
template<class T> void release(T*& value) { if (value) value->Release(); value = nullptr; }
void releaseQueries() {
    for (auto& slot : slots) { release(slot.begin); release(slot.end); slot.record = -1; }
    release(frequencyQuery); release(disjointQuery); currentSlot = -1;
}
void failQuery() { queryErrors.fetch_add(1, std::memory_order_relaxed); }
bool prepareQueries(IDirect3DDevice9* device) {
    releaseQueries();
    if (!device || FAILED(device->CreateQuery(D3DQUERYTYPE_TIMESTAMPFREQ, &frequencyQuery)) ||
        FAILED(device->CreateQuery(D3DQUERYTYPE_TIMESTAMPDISJOINT, &disjointQuery))) return false;
    for (auto& slot : slots)
        if (FAILED(device->CreateQuery(D3DQUERYTYPE_TIMESTAMP, &slot.begin)) ||
            FAILED(device->CreateQuery(D3DQUERYTYPE_TIMESTAMP, &slot.end))) return false;
    return SUCCEEDED(disjointQuery->Issue(D3DISSUE_BEGIN));
}
void pollQueries() {
    // Backend owns all query operations. Zero flags never request a GPU flush.
    for (auto& slot : slots) {
        if (slot.record < 0 || !frames[slot.record].submitEnd) continue;
        auto& record = frames[slot.record];
        std::uint64_t begin = 0, end = 0;
        const auto first = slot.begin->GetData(&begin, sizeof(begin), 0);
        const auto last = slot.end->GetData(&end, sizeof(end), 0);
        if (FAILED(first) || FAILED(last)) { failQuery(); slot.record = -1; ++readyCount; continue; }
        if (first == S_OK && last == S_OK) {
            record.begin = begin; record.end = end; record.firstReady = khz::counter();
            slot.record = -1; ++readyCount;
        }
    }
    gpuReady.store(readyCount, std::memory_order_release);
}
void beginDrain() {
    ended.store(khz::counter(), std::memory_order_relaxed);
    drainDeadline = khz::counter() + khz::frequency() * 2;
    if (supported.load(std::memory_order_relaxed)) {
        if (FAILED(disjointQuery->Issue(D3DISSUE_END)) || FAILED(frequencyQuery->Issue(D3DISSUE_END))) failQuery();
    }
    phase.store(Draining, std::memory_order_release);
}
void finishDrain() {
    if (!supported.load(std::memory_order_relaxed)) { phase.store(Complete, std::memory_order_release); return; }
    pollQueries();
    if (!frequencyReady) {
        std::uint64_t value = 0;
        const auto result = frequencyQuery->GetData(&value, sizeof(value), 0);
        if (result == S_OK) { gpuFrequency.store(value, std::memory_order_relaxed); frequencyReady = true; }
        else if (FAILED(result)) { failQuery(); frequencyReady = true; }
    }
    if (!disjointReady) {
        BOOL value = TRUE;
        const auto result = disjointQuery->GetData(&value, sizeof(value), 0);
        if (result == S_OK) { disjoint.store(value != FALSE, std::memory_order_relaxed); disjointReady = true; }
        else if (FAILED(result)) { failQuery(); disjointReady = true; }
    }
    if (readyCount == frameCount && frequencyReady && disjointReady) phase.store(Complete, std::memory_order_release);
    else if (khz::counter() >= drainDeadline) { failQuery(); phase.store(Aborted, std::memory_order_release); }
}
const char* status() {
    static char text[1024];
    const unsigned state = phase.load(std::memory_order_acquire);
    std::snprintf(text, sizeof(text),
        "{\"phase\":%u,\"epoch\":%llu,\"qpc_frequency\":%llu,\"start_qpc\":%llu,\"end_qpc\":%llu,\"cpu_main\":%u,\"cpu_backend\":%u,\"cpu_overflow\":%u,\"gpu_frames\":%u,\"gpu_ready\":%u,\"gpu_skipped\":%u,\"gpu_supported\":%s,\"gpu_frequency\":%llu,\"disjoint\":%s,\"query_errors\":%u,\"exported\":%s}",
        state, epoch.load(), khz::frequency(), started.load(), ended.load(), cpu[0].published.load(), cpu[1].published.load(),
        cpu[0].overflow.load() + cpu[1].overflow.load(), gpuPublished.load(), gpuReady.load(), gpuSkipped.load(),
        supported.load() ? "true" : "false", gpuFrequency.load(), disjoint.load() ? "true" : "false", queryErrors.load(), exported.empty() ? "false" : "true");
    return text;
}
bool exportCapture() {
    if (phase.load(std::memory_order_acquire) != Complete || !exported.empty()) return false;
    const auto home = std::filesystem::path(KHZ_HomeRoot());
    const auto root = std::filesystem::weakly_canonical(KHZ_ProjectRoot());
    auto relative = std::filesystem::weakly_canonical(home).lexically_relative(root);
    if (relative.empty() || *relative.begin() == ".." || !KHZ_IsBenchmarkHome()) return false;
    const auto path = home / ("render-profile-" + std::to_string(started.load()) + ".json");
    if (std::filesystem::exists(path)) return false;
    std::ofstream file(path, std::ios::binary);
    if (!file) return false;
    file << "{\"schema\":1,\"status\":" << status() << ",\"cpu\":[";
    bool first = true;
    for (unsigned role = 0; role != 2; ++role) {
        const auto count = cpu[role].published.load(std::memory_order_acquire);
        for (unsigned i = 0; i < count; ++i) {
            const auto& event = cpu[role].events[i];
            if (!first) file << ','; first = false;
            file << "{\"role\":" << role << ",\"thread\":" << event.thread << ",\"stage\":\"" << stageNames[event.stage]
                 << "\",\"begin\":" << event.begin << ",\"end\":" << event.end << '}';
        }
    }
    file << "],\"gpu\":[";
    for (unsigned i = 0; i < frameCount; ++i) {
        const auto& frame = frames[i]; if (i) file << ',';
        file << "{\"begin\":" << frame.begin << ",\"end\":" << frame.end << ",\"submit_begin\":" << frame.submitBegin
             << ",\"submit_end\":" << frame.submitEnd << ",\"first_ready\":" << frame.firstReady << '}';
    }
    file << "]}\n"; file.close();
    if (!file) return false;
    exported = path; return true;
}
}
KHZ_RenderProfileScope::KHZ_RenderProfileScope(KHZ_RenderStage stage) : stage_(stage) {
    if (phase.load(std::memory_order_acquire) != Recording) return;
    // The drill-down is for frontend/device contention. The backend streams
    // hundreds of small buffers per frame; retain its whole submission span
    // instead of filling the bounded trace with every backend Lock call.
    if ((stage == KHZ_RenderStage::VertexLock || stage == KHZ_RenderStage::IndexLock) && !Sys_IsMainThread()) return;
    epoch_ = epoch.load(); begin_ = khz::counter();
}
KHZ_RenderProfileScope::~KHZ_RenderProfileScope() {
    if (!begin_ || epoch_ != epoch.load() || phase.load(std::memory_order_acquire) != Recording) return;
    const unsigned role = Sys_IsMainThread() ? 0 : Sys_IsRenderThread() ? 1 : 2;
    if (role > 1) return;
    auto& buffer = cpu[role];
    if (buffer.written == CpuCapacity) { buffer.overflow.fetch_add(1, std::memory_order_relaxed); return; }
    buffer.events[buffer.written++] = {begin_, khz::counter(), unsigned(stage_), Sys_GetCurrentThreadId()};
    buffer.published.store(buffer.written, std::memory_order_release);
}
void KHZ_RenderProfileBackendBegin(IDirect3DDevice9* device) {
    unsigned state = phase.load(std::memory_order_acquire);
    if (state == Preparing) {
        cpu[1].written = 0; cpu[1].published.store(0); cpu[1].overflow.store(0);
        frameCount = readyCount = 0; currentSlot = -1; frequencyReady = disjointReady = false;
        gpuPublished.store(0); gpuReady.store(0); gpuSkipped.store(0); queryErrors.store(0);
        gpuFrequency.store(0); disjoint.store(true);
        const bool available = prepareQueries(device);
        if (!available) releaseQueries();
        supported.store(available, std::memory_order_relaxed);
        const auto now = khz::counter(); started.store(now); ended.store(0);
        deadline = now + khz::frequency() * durationMs.load() / 1000;
        phase.store(Recording, std::memory_order_release); state = Recording;
    }
    if (state == Draining) { finishDrain(); return; }
    if (state != Recording) return;
    if (khz::counter() >= deadline) { beginDrain(); finishDrain(); return; }
    if (!supported.load(std::memory_order_relaxed)) return;
    pollQueries();
    if (frameCount == GpuCapacity) { gpuSkipped.fetch_add(1); return; }
    for (unsigned index = 0; index < QueryCapacity; ++index) {
        auto& slot = slots[index]; if (slot.record >= 0) continue;
        auto& record = frames[frameCount]; record = {}; record.submitBegin = khz::counter();
        if (FAILED(slot.begin->Issue(D3DISSUE_END))) { failQuery(); return; }
        slot.record = frameCount++; currentSlot = index;
        gpuPublished.store(frameCount, std::memory_order_release); return;
    }
    gpuSkipped.fetch_add(1);
}
void KHZ_RenderProfileDrawEnd() {
    if (currentSlot < 0) return;
    auto& slot = slots[currentSlot];
    if (FAILED(slot.end->Issue(D3DISSUE_END))) failQuery();
    frames[slot.record].submitEnd = khz::counter(); currentSlot = -1;
}
void KHZ_RenderProfileFrameEnd() {
    if (phase.load(std::memory_order_acquire) == Draining) finishDrain();
}
void KHZ_RenderProfileShutdown() {
    // Native graphics teardown has already joined the backend and owns device lifetime.
    if (phase.load() == Recording || phase.load() == Preparing || phase.load() == Draining)
        phase.store(Aborted, std::memory_order_release);
    releaseQueries();
}
const char* KHZ_RenderProfileProbe(const char* action) {
    iassert(Sys_IsMainThread());
    if (!action || !Dvar_GetBool("khz_benchmark") || !Dvar_GetBool("khz_test_controls"))
        return "{\"error\":\"requires isolated benchmark and test controls\"}";
    if (!std::strcmp(action, "status")) return status();
    if (!std::strcmp(action, "export")) return exportCapture() ? status() : "{\"error\":\"capture incomplete or already exported\"}";
    if (!std::strncmp(action, "start ", 6)) {
        char* end = nullptr; const auto value = std::strtoul(action + 6, &end, 10);
        if (end == action + 6 || *end || value < 1000 || value > 10000) return "{\"error\":\"duration must be 1000..10000 ms\"}";
        const auto state = phase.load(std::memory_order_acquire);
        if (state != Idle && state != Complete && state != Aborted) return "{\"error\":\"capture already active\"}";
        cpu[0].written = 0; cpu[0].published.store(0); cpu[0].overflow.store(0);
        exported.clear(); durationMs.store(static_cast<unsigned>(value)); epoch.fetch_add(1);
        phase.store(Preparing, std::memory_order_release); return status();
    }
    return "{\"error\":\"expected start, status or export\"}";
}
void KHZ_RenderProfileInit() {
    static cmd_function_s command{};
    Cmd_AddCommandInternal("khz_render_profile", [] {
        char action[64];
        if (Cmd_Argc() == 2) std::snprintf(action, sizeof(action), "%s", Cmd_Argv(1));
        else if (Cmd_Argc() == 3) std::snprintf(action, sizeof(action), "%s %s", Cmd_Argv(1), Cmd_Argv(2));
        else std::snprintf(action, sizeof(action), "status");
        Com_Printf(CON_CHANNEL_DONT_FILTER, "KHZ_RENDER_PROFILE %s\n", KHZ_RenderProfileProbe(action));
    }, &command);
}
