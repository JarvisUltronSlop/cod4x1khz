#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace khz::host_checkpoint {
enum class ChannelFailure : unsigned { None, IncomingFragment, OutgoingFragment, RetainedBounds, InactiveHuman, Download };
struct Channel {
    int fragmentLength = 0, unsentLength = 0, unsentBufferSize = 0;
    bool unsentFragments = false, active = false, downloading = false, downloadHandle = false, downloadingWWW = false;
};
inline ChannelFailure channelFailure(const Channel& value) {
    if (value.fragmentLength) return ChannelFailure::IncomingFragment;
    if (value.unsentFragments) return ChannelFailure::OutgoingFragment;
    // Native Netchan_TransmitNextFragment clears unsentFragments at completion,
    // retaining unsentLength. The latter remains a bounded historical extent.
    if (value.unsentLength < 0 || value.unsentBufferSize < 0 || value.unsentLength > value.unsentBufferSize)
        return ChannelFailure::RetainedBounds;
    if (!value.active) return ChannelFailure::InactiveHuman;
    if (value.downloading || value.downloadHandle || value.downloadingWWW) return ChannelFailure::Download;
    return ChannelFailure::None;
}
inline const char* channelFailureName(ChannelFailure value) {
    switch (value) {
    case ChannelFailure::IncomingFragment: return "incoming_fragment";
    case ChannelFailure::OutgoingFragment: return "outgoing_fragment";
    case ChannelFailure::RetainedBounds: return "retained_bounds";
    case ChannelFailure::InactiveHuman: return "inactive_human";
    case ChannelFailure::Download: return "download";
    default: return "none";
    }
}
enum Failure : std::uint32_t {
    Benchmark = 1u, Configuration = 2u, Ready = 4u, Fixture = 8u, Session = 16u,
    ServerRunning = 32u, Game = 64u, InsideServerFrame = 128u, Clients = 256u,
    Plugins = 512u, WebAdmin = 1024u, HumanCount = 2048u, Queues = 4096u,
    PauseState = 8192u, NativeChannel = 16384u
};
struct State {
    bool benchmark = false, configured = false, ready = false, fixture = false, session = false;
    bool running = false, game = false, inServerFrame = false, clients = false;
    bool pluginsAbsent = false, webadminAbsent = false, queuesQuiet = false, pauseStateValid = false;
    unsigned humans = 0;
    ChannelFailure channel = ChannelFailure::None;
};
inline std::uint32_t failureMask(const State& value) {
    return (!value.benchmark ? Benchmark : 0u) | (!value.configured ? Configuration : 0u) |
        (!value.ready ? Ready : 0u) | (!value.fixture ? Fixture : 0u) | (!value.session ? Session : 0u) |
        (!value.running ? ServerRunning : 0u) | (!value.game ? Game : 0u) |
        (value.inServerFrame ? InsideServerFrame : 0u) | (!value.clients ? Clients : 0u) |
        (!value.pluginsAbsent ? Plugins : 0u) | (!value.webadminAbsent ? WebAdmin : 0u) |
        (value.humans != 1 ? HumanCount : 0u) | (!value.queuesQuiet ? Queues : 0u) |
        (!value.pauseStateValid ? PauseState : 0u) |
        (value.channel != ChannelFailure::None ? NativeChannel : 0u);
}
// The authenticated host packet reserves 160 bytes for its error. Compact
// field labels and hex identities retain every observed value even at the
// native integer bounds; m is the Failure bit mask above, p0/w0 mean absent.
inline int channelDiagnostic(char* output, std::size_t capacity, const State& state, int slot, const Channel& channel) {
    return std::snprintf(output, capacity,
        "server m=%u slot=%i why=%s frag=%i pend=%u extent=%i cap=%i active=%u dl=%u/%u/%u h=%u",
        failureMask(state), slot, channelFailureName(state.channel), channel.fragmentLength,
        unsigned(channel.unsentFragments), channel.unsentLength, channel.unsentBufferSize,
        unsigned(channel.active), unsigned(channel.downloading), unsigned(channel.downloadHandle),
        unsigned(channel.downloadingWWW), state.humans);
}
inline int stateDiagnostic(char* output, std::size_t capacity, const State& state,
    std::uint64_t fixture, std::uint64_t requestedSession, std::uint64_t currentSession, bool paused) {
    return std::snprintf(output, capacity,
        "server m=%u f=%llx s=%llx/%llx h=%u q=%u p0=%u w0=%u pause=%u",
        failureMask(state), static_cast<unsigned long long>(fixture),
        static_cast<unsigned long long>(requestedSession), static_cast<unsigned long long>(currentSession),
        state.humans, unsigned(state.queuesQuiet), unsigned(state.pluginsAbsent),
        unsigned(state.webadminAbsent), unsigned(paused));
}
}
