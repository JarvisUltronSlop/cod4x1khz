#pragma once
#include <server_capture.hpp>

struct client_t;
struct usercmd_s;

// Dedicated game-owner calls only. No socket/I/O worker may inspect mutable
// native state or drain this observer. Begin/end/status/export are outside ticks.
bool KHZ_ServerCaptureBegin(std::uint64_t fixture, unsigned ticks);
bool KHZ_ServerCaptureEnd();
bool KHZ_ServerCapturePop(khz::server_capture::TickRecord* record);
const char* KHZ_ServerCaptureStatusJSON();
// Read one retained complete/incomplete record outside a native gameplay tick.
// Named JSON has no native padding, pointers, credentials, or restore claims.
const char* KHZ_ServerCaptureTickJSON(unsigned ordinal);
// Diagnostic-only atomic JSONL export of every retained immutable record.
// Requires an isolated benchmark home and a complete inactive tape outside
// authoritative ticks; this neither drains records nor restores a world.
const char* KHZ_ServerCaptureExportJSON();
void KHZ_ServerCaptureInvalidateLifecycle();

// Begin at the start of KHZ_ApplyCommands, after the server incremented svs.time
// and prepared bots. Record final post-plugin selected input before lastUsercmd
// and ClientThink. End after SV_PostFrame completes the authoritative tick.
void KHZ_ServerCaptureBeginTick(std::uint64_t session, std::uint64_t tick, int serverTime);
void KHZ_ServerCaptureRecord(client_t* client, const usercmd_s* selected,
    std::uint64_t latchSequenceBefore, std::uint64_t latchSequenceAfter,
    std::uint64_t appliedSequence, std::uint64_t receivedSequence,
    std::uint64_t viewTick, unsigned consumedCommands);
void KHZ_ServerCaptureEndTick(std::uint64_t session, std::uint64_t tick, int serverTime);
