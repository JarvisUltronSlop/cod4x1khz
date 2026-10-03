#include <universal/q_shared.h>
#include "khz_server_capture.h"
#include "khz_runtime.h"
#include "khz_admin_server.h"
#include "khz_bots.h"
#include "khz_paths.h"
#include <server_mp/server_mp.h>
#include <game_mp/g_main_mp.h>
#include <client_mp/client_mp.h>
#include <qcommon/msg_mp.h>
#include <qcommon/qcommon.h>
#include <qcommon/threads.h>
#include <cstdio>
#include <cstring>
#include <Windows.h>
#include <bcrypt.h>

namespace {
using namespace khz::server_capture;
static_assert(MaxSlots == KHZ_MaxMatchClients, "record every native match slot");
static_assert(MaxConsumedCommands == khz::InputHistory, "real bounded command drain capacity");
TickRecord records[MaxTicks]; // Raw BSS: no huge aggregate startup temporary.
Tape<MaxTicks> tape(records);
std::uint64_t nativeGeneration(const client_t* client) {
    return KHZ_IsNativeBot(client) ? KHZ_BotClientGeneration(client) : KHZ_AdminClientGeneration(client);
}
std::uint64_t activeSlots() {
    if (!svs.initialized || !sv_maxclients || sv_maxclients->current.integer < 0 || sv_maxclients->current.integer > 64)
        return UINT64_MAX;
    std::uint64_t mask = 0;
    for (int slot = 0; slot < sv_maxclients->current.integer; ++slot)
        if (svs.clients[slot].header.state == CS_ACTIVE) mask |= 1ull << slot;
    return mask;
}
const char* failureName(Failure failure) {
    switch (failure) {
    case Failure::None: return "none";
    case Failure::TickAlreadyOpen: return "tick_already_open";
    case Failure::TickNotOpen: return "tick_not_open";
    case Failure::TickContinuity: return "authoritative_tick_discontinuity";
    case Failure::TimeContinuity: return "native_millisecond_discontinuity";
    case Failure::Lifecycle: return "native_session_or_active_slots_changed";
    case Failure::ActiveSlotRange: return "active_slot_outside_16_player_fixture";
    case Failure::InactiveSlot: return "input_record_for_inactive_slot";
    case Failure::DuplicateSlot: return "duplicate_slot_consumption";
    case Failure::MissingSlot: return "missing_active_slot_consumption";
    case Failure::SlotGeneration: return "native_slot_generation_changed_or_missing";
    case Failure::InvalidCommand: return "invalid_selected_native_command";
    case Failure::InvalidInputMetadata: return "invalid_latch_or_ack_metadata";
    case Failure::Overflow: return "fixed_tape_capacity_exhausted";
    case Failure::IncompleteTick: return "capture_ended_inside_native_tick";
    case Failure::EndedEarly: return "capture_ended_before_target";
    }
    return "unknown";
}
const char* sourceName(Source source) {
    return source == Source::NativeBot ? "native_bot" : source == Source::Queued ? "queued" : "held";
}
}

bool KHZ_ServerCaptureBegin(std::uint64_t fixture, unsigned ticks) {
    iassert(Sys_IsMainThread());
    if (!Dvar_GetBool("khz_benchmark") || !com_sv_running || !com_sv_running->current.enabled ||
        sv.state != SS_GAME || !sv_maxclients || sv_maxclients->current.integer < 1 ||
        sv_maxclients->current.integer > static_cast<int>(MaxSlots) || !sv_serverid) return false;
    return tape.begin({fixture, KHZ_CurrentServerSession(), ticks, sv_serverid->current.integer});
}
bool KHZ_ServerCaptureEnd() { iassert(Sys_IsMainThread()); tape.end(); return true; }
void KHZ_ServerCaptureInvalidateLifecycle() { iassert(Sys_IsMainThread()); tape.invalidateLifecycle(); }
void KHZ_ServerCaptureBeginTick(std::uint64_t session, std::uint64_t tick, int serverTime) {
    iassert(Sys_IsMainThread());
    if (tape.active() || tape.tickOpen()) tape.beginTick(session, tick, serverTime, activeSlots());
}
void KHZ_ServerCaptureRecord(client_t* client, const usercmd_s* selected,
    std::uint64_t before, std::uint64_t after, std::uint64_t applied,
    std::uint64_t received, std::uint64_t viewTick, unsigned consumedCommands) {
    iassert(Sys_IsMainThread());
    if (!tape.active() && !tape.tickOpen()) return;
    if (!client || !selected || client < &svs.clients[0] || client >= &svs.clients[64]) {
        tape.reject(Failure::InvalidCommand); return;
    }
    const auto slot = static_cast<unsigned>(client - svs.clients);
    if (slot >= MaxSlots || client->header.state != CS_ACTIVE || client->gentity != &g_entities[slot] ||
        !level.clients || !g_entities[slot].client || g_entities[slot].client != &level.clients[slot]) {
        tape.reject(Failure::InactiveSlot); return;
    }
    SlotRecord record{};
    record.slot = slot; record.generation = nativeGeneration(client);
    const bool bot = KHZ_IsNativeBot(client);
    record.source = bot ? Source::NativeBot : consumedCommands ? Source::Queued : Source::Held;
    record.inputSequenceBefore = bot ? KHZ_ServerTickNumber() : before;
    record.inputSequenceAfter = bot ? KHZ_ServerTickNumber() : after;
    record.inputAdvanced = record.inputSequenceAfter > record.inputSequenceBefore;
    record.appliedSequence = applied; record.receivedSequence = received;
    record.viewTick = viewTick; record.consumedCommands = consumedCommands;
    record.entityUseCount = g_entities[slot].useCount;
    record.lastConnectTime = client->lastConnectTime; record.lastSpawnTime = level.clients[slot].lastSpawnTime;
    record.serverId = client->serverId; record.scriptId = client->scriptId;
    auto& output = record.command;
    output.serverTime = selected->serverTime; output.buttons = selected->buttons;
    for (unsigned index = 0; index < 3; ++index) output.angles[index] = selected->angles[index];
    output.weapon = selected->weapon; output.offHandIndex = selected->offHandIndex;
    output.forwardmove = static_cast<std::int8_t>(selected->forwardmove);
    output.rightmove = static_cast<std::int8_t>(selected->rightmove);
    output.meleeChargeYaw = selected->meleeChargeYaw; output.meleeChargeDist = selected->meleeChargeDist;
    for (unsigned index = 0; index < 2; ++index) output.selectedLocation[index] = static_cast<std::int8_t>(selected->selectedLocation[index]);
    tape.record(record);
}
void KHZ_ServerCaptureEndTick(std::uint64_t session, std::uint64_t tick, int serverTime) {
    iassert(Sys_IsMainThread());
    if (!tape.active() && !tape.tickOpen()) return;
    std::uint64_t generations[MaxSlots]{};
    const auto active = activeSlots();
    for (unsigned slot = 0; slot < MaxSlots; ++slot)
        if (active & (1ull << slot)) generations[slot] = nativeGeneration(&svs.clients[slot]);
    tape.endTick(session, tick, serverTime, active, generations);
}
bool KHZ_ServerCapturePop(TickRecord* record) { iassert(Sys_IsMainThread()); return record && tape.pop(*record); }
const char* KHZ_ServerCaptureStatusJSON() {
    iassert(Sys_IsMainThread()); static char output[1536]; const auto status = tape.status();
    std::snprintf(output, sizeof(output),
        "{\"schema\":1,\"fixture\":%llu,\"session\":%llu,\"server_id\":%i,\"target_ticks\":%u,\"capacity_ticks\":%u,\"capacity_slots\":%u,\"storage_bytes\":%zu,\"recorded\":%u,\"drained\":%u,\"queued\":%u,\"invalid\":%u,\"rejected\":%u,\"first_tick\":%llu,\"last_tick\":%llu,\"active\":%s,\"tick_open\":%s,\"finished\":%s,\"complete\":%s,\"export_complete\":%s,\"verified_restore_conditions\":0,\"full_world_replay_verified\":false,\"failure\":\"%s\",\"scope\":\"actual post-plugin authoritative input consumption; no world/script/RNG restoration or renderer replay attestation\"}",
        status.request.fixture, status.request.session, status.request.serverId, status.request.ticks,
        MaxTicks, MaxSlots, sizeof(records), status.recorded, status.drained, status.queued, status.invalid, status.rejected,
        status.firstTick, status.lastTick, status.active ? "true" : "false", status.tickOpen ? "true" : "false",
        status.finished ? "true" : "false", status.complete ? "true" : "false", status.exportComplete ? "true" : "false", failureName(status.failure));
    return output;
}
const char* KHZ_ServerCaptureTickJSON(unsigned ordinal) {
    iassert(Sys_IsMainThread()); static char output[16384];
    const auto* record = tape.find(ordinal);
    if (!record) { std::snprintf(output, sizeof(output), "{\"schema\":1,\"found\":false,\"ordinal\":%u}", ordinal); return output; }
    int written = std::snprintf(output, sizeof(output),
        "{\"schema\":1,\"found\":true,\"fixture\":%llu,\"session\":%llu,\"ordinal\":%llu,\"tick\":%llu,\"server_time\":%i,\"server_id\":%i,\"active_mask_before\":%u,\"active_mask_after\":%u,\"recorded_mask\":%u,\"complete\":%s,\"verified_restore_conditions\":0,\"failure\":\"%s\",\"slots\":[",
        record->fixture, record->session, record->ordinal, record->tick, record->serverTime, record->serverId,
        record->activeMaskBefore, record->activeMaskAfter, record->recordedMask,
        record->complete ? "true" : "false", failureName(record->failure));
    bool first = true;
    for (unsigned slot = 0; slot < MaxSlots; ++slot) {
        if (!(record->recordedMask & (1u << slot))) continue;
        const auto& input = record->slots[slot]; const auto& command = input.command;
        if (written < 0 || static_cast<unsigned>(written) >= sizeof(output)) break;
        written += std::snprintf(output + written, sizeof(output) - static_cast<unsigned>(written),
            "%s{\"slot\":%u,\"generation\":%llu,\"source\":\"%s\",\"input_sequence_before\":%llu,\"input_sequence_after\":%llu,\"input_advanced\":%s,\"applied_sequence\":%llu,\"received_sequence\":%llu,\"view_tick\":%llu,\"consumed_commands\":%u,\"entity_use_count\":%i,\"last_connect_time\":%i,\"last_spawn_time\":%i,\"server_id\":%i,\"script_id\":%u,\"command\":{\"server_time\":%i,\"buttons\":%i,\"angles\":[%i,%i,%i],\"weapon\":%u,\"offhand\":%u,\"forward\":%i,\"right\":%i,\"melee_yaw\":%.9g,\"melee_distance\":%u,\"selected_location\":[%i,%i]}}",
            first ? "" : ",", input.slot, input.generation, sourceName(input.source), input.inputSequenceBefore,
            input.inputSequenceAfter, input.inputAdvanced ? "true" : "false", input.appliedSequence, input.receivedSequence,
            input.viewTick, input.consumedCommands, input.entityUseCount, input.lastConnectTime, input.lastSpawnTime,
            input.serverId, static_cast<unsigned>(input.scriptId), command.serverTime, command.buttons, command.angles[0], command.angles[1],
            command.angles[2], static_cast<unsigned>(command.weapon), static_cast<unsigned>(command.offHandIndex), command.forwardmove, command.rightmove,
            command.meleeChargeYaw, static_cast<unsigned>(command.meleeChargeDist), command.selectedLocation[0], command.selectedLocation[1]);
        first = false;
    }
    if (written < 0 || static_cast<unsigned>(written) + 3 >= sizeof(output)) {
        std::snprintf(output, sizeof(output), "{\"schema\":1,\"found\":true,\"complete\":false,\"failure\":\"json_capacity\",\"verified_restore_conditions\":0}");
        return output;
    }
    std::snprintf(output + written, sizeof(output) - static_cast<unsigned>(written), "]}");
    return output;
}

const char* KHZ_ServerCaptureExportJSON() {
    iassert(Sys_IsMainThread());
    static char output[1536];
    const auto status = tape.status();
    const char* failure = "none";
    bool committed = false;
    bool temporaryOwned = false;
    unsigned recordsWritten = 0;
    std::uint64_t bytesWritten = 0;
    LARGE_INTEGER started{}, finished{}, frequency{};
    QueryPerformanceCounter(&started); QueryPerformanceFrequency(&frequency);
    char digest[65]{};
    HANDLE file = INVALID_HANDLE_VALUE;
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    char temporary[MAX_PATH]{}, destination[MAX_PATH]{};
    const auto safeAncestors = [](const char* path) {
        char absolute[MAX_PATH];
        const auto count = GetFullPathNameA(path, sizeof(absolute), absolute, nullptr);
        if (count < 3 || count >= sizeof(absolute) || absolute[1] != ':' || absolute[2] != '\\') return false;
        const char first = absolute[3]; absolute[3] = 0;
        const auto rootAttributes = GetFileAttributesA(absolute); absolute[3] = first;
        if (rootAttributes == INVALID_FILE_ATTRIBUTES || !(rootAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
            (rootAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
        // This private local project deliberately excludes UNC, long-path and
        // reparse roots. Inspect every ancestor, not just the final directory.
        for (unsigned i = 3; i <= count; ++i) {
            if (absolute[i] != '\\' && absolute[i] != 0) continue;
            const char delimiter = absolute[i]; absolute[i] = 0;
            const auto attributes = GetFileAttributesA(absolute);
            absolute[i] = delimiter;
            if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY) ||
                (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
        }
        return true;
    };
    do {
        // The actual game owner has the only tape reference. This command is
        // outside authoritative ticks; immutable records are never popped or
        // overwritten here, and no gameplay state is copied to an I/O worker.
        if (!Dvar_GetBool("khz_benchmark") || !KHZ_IsBenchmarkHome() || !com_sv_running ||
            !com_sv_running->current.enabled || sv.state != SS_GAME || com_inServerFrame) {
            failure = "requires_inactive_benchmark_diagnostic"; break;
        }
        if (status.active || status.tickOpen || !status.finished || !status.complete ||
            !status.request.fixture || !status.request.ticks || status.request.ticks > MaxTicks ||
            status.recorded != status.request.ticks || status.invalid || status.rejected ||
            status.failure != Failure::None || status.request.session != KHZ_CurrentServerSession() ||
            !sv_serverid || status.request.serverId != sv_serverid->current.integer) {
            failure = "requires_complete_current_immutable_tape"; break;
        }
        char benchmarks[MAX_PATH], directory[MAX_PATH];
        const int a = std::snprintf(benchmarks, sizeof(benchmarks), "%s\\benchmarks", KHZ_RuntimeRoot());
        const int b = std::snprintf(directory, sizeof(directory), "%s\\main", KHZ_HomeRoot());
        const int c = std::snprintf(destination, sizeof(destination), "%s\\khz_server_capture.jsonl", directory);
        const int d = std::snprintf(temporary, sizeof(temporary), "%s\\khz_server_capture.%lu.%llu.tmp", directory,
            GetCurrentProcessId(), status.request.fixture);
        if (a < 0 || a >= static_cast<int>(sizeof(benchmarks)) || b < 0 || b >= static_cast<int>(sizeof(directory)) ||
            c < 0 || c >= static_cast<int>(sizeof(destination)) || d < 0 || d >= static_cast<int>(sizeof(temporary)) ||
            !safeAncestors(KHZ_RuntimeRoot()) || !safeAncestors(benchmarks) ||
            !safeAncestors(KHZ_HomeRoot()) || !safeAncestors(directory)) {
            failure = "benchmark_path_not_safe"; break;
        }
        const auto previous = GetFileAttributesA(destination);
        if (previous != INVALID_FILE_ATTRIBUTES && (previous & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) {
            failure = "export_destination_not_regular_file"; break;
        }
        if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0 ||
            BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) < 0) {
            failure = "sha256_initialization"; break;
        }
        // CREATE_NEW prevents overwriting a stale or substituted temp file.
        file = CreateFileA(temporary, GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) { failure = "temporary_create"; temporary[0] = 0; break; }
        temporaryOwned = true;
        for (unsigned ordinal = 1; ordinal <= status.recorded; ++ordinal) {
            const auto* record = tape.find(ordinal);
            if (!record || !record->complete || record->failure != Failure::None || record->ordinal != ordinal ||
                record->fixture != status.request.fixture || record->session != status.request.session ||
                record->serverId != status.request.serverId || record->tick != status.firstTick + ordinal - 1) {
                failure = "immutable_record_identity"; break;
            }
            const char* json = KHZ_ServerCaptureTickJSON(ordinal);
            const auto length = std::strlen(json);
            if (!length || length >= 16384 || std::strstr(json, "\"complete\":false")) {
                failure = "record_json_capacity"; break;
            }
            DWORD written = 0;
            if (!WriteFile(file, json, static_cast<DWORD>(length), &written, nullptr) || written != length ||
                BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<char*>(json)), static_cast<ULONG>(length), 0) < 0) {
                failure = "record_write_or_hash"; break;
            }
            unsigned char newline = '\n';
            if (!WriteFile(file, &newline, 1, &written, nullptr) || written != 1 ||
                BCryptHashData(hash, &newline, 1, 0) < 0) {
                failure = "newline_write_or_hash"; break;
            }
            bytesWritten += length + 1; ++recordsWritten;
        }
        if (recordsWritten != status.recorded) break;
        unsigned char rawDigest[32];
        if (BCryptFinishHash(hash, rawDigest, sizeof(rawDigest), 0) < 0) { failure = "sha256_finish"; break; }
        static constexpr char hex[] = "0123456789abcdef";
        for (unsigned i = 0; i < sizeof(rawDigest); ++i) { digest[2*i] = hex[rawDigest[i] >> 4]; digest[2*i+1] = hex[rawDigest[i] & 15]; }
        if (!FlushFileBuffers(file)) { failure = "temporary_flush"; break; }
        if (!CloseHandle(file)) { failure = "temporary_close"; break; }
        file = INVALID_HANDLE_VALUE;
        if (!MoveFileExA(temporary, destination, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            failure = "atomic_commit"; break;
        }
        committed = true; temporary[0] = 0;
    } while (false);
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    if (temporaryOwned && *temporary) DeleteFileA(temporary);
    if (hash) BCryptDestroyHash(hash);
    if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    QueryPerformanceCounter(&finished);
    const auto elapsed = frequency.QuadPart > 0 && finished.QuadPart >= started.QuadPart ?
        static_cast<std::uint64_t>((finished.QuadPart - started.QuadPart) * 1000000 / frequency.QuadPart) : 0;
    std::snprintf(output, sizeof(output),
        "{\"schema\":1,\"committed\":%s,\"file\":\"main/khz_server_capture.jsonl\",\"format\":\"jsonl\",\"fixture\":%llu,\"session\":%llu,\"server_id\":%i,\"records\":%u,\"bytes\":%llu,\"sha256\":\"%s\",\"first_tick\":%llu,\"last_tick\":%llu,\"elapsed_us\":%llu,\"failure\":\"%s\",\"full_world_replay_verified\":false,\"scope\":\"measured main-thread off-tick diagnostic stall; atomic export of actual retained input, no world/script/RNG restoration\"}",
        committed ? "true" : "false", status.request.fixture, status.request.session, status.request.serverId,
        recordsWritten, bytesWritten, digest, status.firstTick, status.lastTick, elapsed, failure);
    return output;
}
