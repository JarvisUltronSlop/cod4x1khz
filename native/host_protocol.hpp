#pragma once
#include <cstdint>

namespace khz::hosting {
inline constexpr std::uint32_t WireMagic = 0x484b4831, WireVersion = 2;
inline constexpr unsigned MaxRules = 48, QueueCapacity = 8;
inline constexpr unsigned ReadyTimeout = 120000, StopTimeout = 5000;
inline constexpr auto PipePrefix = L"\\\\.\\pipe\\cod4x1khz-host-";
enum class HostMessage : std::uint32_t {
    Hello = 1,
    Configure,
    Ready,
    Map,
    Restart,
    Stop,
    Failure,
    Command,
    CommandResult,
    CheckpointPause,
    CheckpointResume,
    CheckpointFence,
    Status,
    StatusResult,
    PlayerAction,
    Bots
};
enum class NetworkMode : std::uint32_t { Localhost, Lan, Internet };
enum class PlayerAction : std::uint32_t { Kick, TemporaryBan, PermanentBan, AddAdmin };
struct Rule {
    char name[64]{}, value[128]{};
};
struct Player {
    std::uint64_t generation = 0;
    std::int32_t slot = -1, ping = 0, power = 0;
    std::uint32_t bot = 0;
    char name[64]{}, identity[65]{};
};
// Fixed-width fields and explicit packing are shared by both compiler ABIs.
#pragma pack(push, 8)
struct Packet {
    std::uint32_t magic = WireMagic, version = WireVersion;
    HostMessage message{};
    std::uint32_t sequence = 0, protocol = 10002, pid = 0, request = 0;
    char token[33]{}, map[64]{}, gametype[32]{}, error[160]{};
    std::uint32_t port = 0, dedicated = 0, bots = 0, difficulty = 0, ruleCount = 0;
    Rule rules[MaxRules]{};
    char command[1024]{}, output[2048]{};
    std::uint64_t fixture = 0, session = 0, tick = 0;
    std::uint32_t maxClients = 16, manager = 0, playerCount = 0;
    Player players[16]{};
    std::int32_t targetSlot = -1;
    PlayerAction action{};
    std::uint64_t admission = 0;
    std::uint32_t minutes = 30, power = 10;
};
#pragma pack(pop)
static_assert(sizeof(Packet) < 16384);
} // namespace khz::hosting
