#pragma once
#include <cstdint>

namespace khz::checkpoint {

// These domains are actual independently owned native storage. All must be
// captured and restored; a player-state digest is not a checkpoint provider.
enum class Domain : unsigned {
    NativeClient, OwnerBoundary, Input, Pose, Animation, AimTargets,
    Projectiles, GameplayEffects, RuntimeHistory, Transport, Count
};
inline constexpr unsigned DomainCount = static_cast<unsigned>(Domain::Count);
inline constexpr unsigned AllDomains = (1u << DomainCount) - 1;
enum class Operation : unsigned { Capture, ValidateRestore, Restore, Discard };
enum class Failure : unsigned {
    None, Busy, NotParkedWorker, MissingProvider, InvalidIdentity,
    InputNotQuiescent, InvalidNativeState, ProviderCapture,
    ProviderValidation, ProviderRestore, NoCheckpoint, OwnerFault
};

struct Frame {
    std::uint64_t fixture = 0, session = 0, generation = 0;
    std::uint64_t logicalTick = 0, qpcFrequency = 0;
    unsigned nativeInputMilliseconds = 0;
    int currentSnapshotMessage = 0, nextSnapshotMessage = 0;
};
inline bool valid(const Frame& frame) {
    return frame.fixture && frame.session && frame.generation && frame.qpcFrequency;
}
inline bool sameLifetime(const Frame& saved, const Frame& current) {
    return valid(saved) && valid(current) && saved.fixture == current.fixture &&
        saved.session == current.session && saved.generation == current.generation &&
        saved.qpcFrequency == current.qpcFrequency;
}

// Replay rebases receipt QPCs to a fresh wall-clock anchor while native input
// milliseconds and logical simulation ticks continue from the restored state.
// Tick/subtick conversion uses rational arithmetic, including non-divisible QPC
// frequencies. A subtick is strictly less than the one-millisecond game tick.
inline bool receiptDeadline(std::uint64_t qpcEpoch, std::uint64_t frequency,
    std::uint64_t tickOffset, unsigned microseconds, std::uint64_t* output) {
    if (!output || !frequency || microseconds >= 1000) return false;
    constexpr std::uint64_t limit = UINT64_MAX;
    const auto seconds = tickOffset / 1000;
    const auto remainder = (tickOffset % 1000) * 1000 + microseconds;
    if (seconds > limit / frequency || remainder > limit / frequency) return false;
    const auto whole = seconds * frequency;
    const auto numerator = remainder * frequency;
    const auto fraction = numerator / 1000000 + (numerator % 1000000 != 0);
    if (whole > limit - fraction || qpcEpoch > limit - whole - fraction) return false;
    *output = qpcEpoch + whole + fraction;
    return true;
}

} // namespace khz::checkpoint
