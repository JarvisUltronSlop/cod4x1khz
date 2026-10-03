#pragma once
#include <cstdint>

// Authoritative main owner only. Nested phases are observations, so their
// totals overlap; the outer fixed-tick measurement remains the acceptance gate.
enum class KHZ_ServerPhase : unsigned {
    Preparation, PreFrame, Input, Gameplay, PostFrame, Snapshots, PlayerTriggers,
    AnimationClock, DueScripts, PlayerNotifies, AnimationUpdate, Entities,
    ClientEndFrame, FrameEnd, Compression, Archive, Count
};
void KHZ_ServerMetricsReset();
void KHZ_ServerMetricsBeginTick(std::uint64_t tick);
void KHZ_ServerMetricsEndTick();
bool KHZ_ServerMetricsActive();
bool KHZ_ServerMetricsScriptActive();
void KHZ_ServerMetricsRecord(KHZ_ServerPhase phase, std::uint64_t elapsed);
const char* KHZ_ServerMetricsJSON();
std::uint64_t KHZ_ServerMetricsCounter();
void KHZ_ServerMetricsScriptSlice(const char* position, std::uint64_t elapsed);
class KHZ_ServerScriptSliceScope {
public:
    explicit KHZ_ServerScriptSliceScope(const char* position)
        : position_(position), active_(KHZ_ServerMetricsScriptActive()), start_(active_ ? KHZ_ServerMetricsCounter() : 0) {}
    ~KHZ_ServerScriptSliceScope() { if (active_) KHZ_ServerMetricsScriptSlice(position_, KHZ_ServerMetricsCounter() - start_); }
private:
    const char* position_;
    bool active_;
    std::uint64_t start_;
};
class KHZ_ServerPhaseScope {
public:
    explicit KHZ_ServerPhaseScope(KHZ_ServerPhase phase)
        : phase_(phase), active_(KHZ_ServerMetricsActive()), start_(active_ ? KHZ_ServerMetricsCounter() : 0) {}
    ~KHZ_ServerPhaseScope() { if (active_) KHZ_ServerMetricsRecord(phase_, KHZ_ServerMetricsCounter() - start_); }
    KHZ_ServerPhaseScope(const KHZ_ServerPhaseScope&) = delete;
    KHZ_ServerPhaseScope& operator=(const KHZ_ServerPhaseScope&) = delete;
private:
    KHZ_ServerPhase phase_;
    bool active_;
    std::uint64_t start_;
};
