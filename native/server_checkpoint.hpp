#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace khz::server_checkpoint {

// Every bit names storage that must actually be restored. Neither a digest nor
// a snapshot archive is a provider for the mutable authoritative world.
enum class Domain : unsigned {
    World, Graphs, ScriptVM, ScriptAllocators, Schedule, RNG, Network, Runtime,
    Bots, Admin, Animation, Collision, Physics, Dvars, Count
};
inline constexpr unsigned DomainCount = unsigned(Domain::Count);
inline constexpr unsigned AllDomains = (1u << DomainCount) - 1;
inline constexpr unsigned MaxParts = 8;
inline constexpr unsigned RequiredParts[DomainCount] = {
    1, 3, 3, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1
};
enum class Failure : unsigned {
    None, Busy, NotPausedOwner, MissingObserver, MissingProvider, InvalidIdentity,
    LifetimeChanged, CaptureRejected, ValidationRejected, NoCheckpoint
};
enum class Phase : unsigned { Empty, Capturing, Pinned, Validating, Restoring, Discarding };

struct Frame {
    std::uint64_t fixture = 0, session = 0, mapGeneration = 0, logicalTick = 0;
    std::uint64_t fenceGeneration = 0;
    int nativeTime = 0, serverId = 0;
    unsigned mapChecksum = 0;
    std::uintptr_t serverAddress = 0, clientsAddress = 0, entitiesAddress = 0;
    std::uintptr_t scriptProgramAddress = 0, mtBufferAddress = 0, hunkBaseAddress = 0;
    std::size_t hunkLowUsed = 0, hunkHighUsed = 0;
};
inline bool valid(const Frame& f) {
    return f.fixture && f.session && f.mapGeneration && f.fenceGeneration &&
        f.serverAddress && f.clientsAddress && f.entitiesAddress &&
        f.scriptProgramAddress && f.mtBufferAddress && f.hunkBaseAddress;
}
inline bool sameLifetime(const Frame& a, const Frame& b) {
    return valid(a) && valid(b) && a.fixture == b.fixture && a.session == b.session &&
        a.mapGeneration == b.mapGeneration && a.fenceGeneration == b.fenceGeneration &&
        a.serverId == b.serverId && a.mapChecksum == b.mapChecksum &&
        a.serverAddress == b.serverAddress && a.clientsAddress == b.clientsAddress &&
        a.entitiesAddress == b.entitiesAddress && a.scriptProgramAddress == b.scriptProgramAddress &&
        a.mtBufferAddress == b.mtBufferAddress && a.hunkBaseAddress == b.hunkBaseAddress &&
        a.hunkLowUsed == b.hunkLowUsed && a.hunkHighUsed == b.hunkHighUsed;
}

struct Provider {
    Domain domain;
    unsigned part;
    std::size_t storageBytes;
    bool (*capture)(const Frame&, char*, unsigned);
    bool (*validateRestore)(const Frame&, char*, unsigned);
    // Commit cannot fail or invoke gameplay, allocation, refcount callbacks or
    // external I/O. ALL providers validate before the first live write.
    void (*restore)(const Frame&);
    void (*discard)();
};

struct Range { Domain domain; std::uintptr_t base; std::size_t bytes; bool mapLifetime = false; };
class Ranges {
public:
    // Dedicated animation contributes 72 separately allocated native trees.
    // Keep exact restored spans, rather than claiming the surrounding hunk.
    static constexpr unsigned Capacity = 256;
    bool add(Domain domain, const void* base, std::size_t bytes) {
        const auto address = reinterpret_cast<std::uintptr_t>(base);
        if (unsigned(domain) >= DomainCount || !address || !bytes || bytes > UINTPTR_MAX - address) return false;
        for (unsigned i = 0; i < count_; ++i)
            if (values_[i].domain == domain && values_[i].base == address && values_[i].bytes == bytes) return true;
        if (count_ == Capacity) return false;
        values_[count_++] = {domain, address, bytes}; return true;
    }
    bool contains(Domain domain, const void* pointer, std::size_t bytes) const {
        const auto address = reinterpret_cast<std::uintptr_t>(pointer);
        if (!address || !bytes || bytes > UINTPTR_MAX - address) return false;
        for (unsigned i = 0; i < count_; ++i) {
            const auto& r = values_[i];
            if (r.domain == domain && address >= r.base && bytes <= r.bytes && address - r.base <= r.bytes - bytes) return true;
        }
        return false;
    }
    bool containsAny(const void* pointer, std::size_t bytes) const {
        for (unsigned d = 0; d < DomainCount; ++d) if (contains(Domain(d), pointer, bytes)) return true;
        return false;
    }
    // Replace one provider's exact current-map allocations atomically. Validate
    // the whole replacement before removing the previous map's live spans.
    // Static arenas and other providers retain their independent registrations.
    bool replaceMap(Domain domain, const void* const* bases, const std::size_t* bytes, unsigned count) {
        if (unsigned(domain) >= DomainCount || count > Capacity || (count && (!bases || !bytes))) return false;
        unsigned retained = 0;
        for (unsigned i = 0; i < count_; ++i)
            if (!(values_[i].mapLifetime && values_[i].domain == domain)) ++retained;
        if (count > Capacity - retained) return false;
        for (unsigned i = 0; i < count; ++i) {
            const auto address = reinterpret_cast<std::uintptr_t>(bases[i]);
            if (!address || !bytes[i] || bytes[i] > UINTPTR_MAX - address) return false;
            for (unsigned j = 0; j < i; ++j)
                if (bases[j] == bases[i]) return false;
        }
        unsigned at = 0;
        for (unsigned i = 0; i < count_; ++i)
            if (!(values_[i].mapLifetime && values_[i].domain == domain)) values_[at++] = values_[i];
        for (unsigned i = 0; i < count; ++i)
            values_[at++] = {domain, reinterpret_cast<std::uintptr_t>(bases[i]), bytes[i], true};
        count_ = at; return true;
    }
    void resetMap() {
        unsigned at = 0;
        for (unsigned i = 0; i < count_; ++i) if (!values_[i].mapLifetime) values_[at++] = values_[i];
        count_ = at;
    }
private:
    Range values_[Capacity]{};
    unsigned count_ = 0;
};

class Branch {
public:
    void reset() { rejected_ = false; reason_[0] = 0; }
    void reject(const char* reason) {
        rejected_ = true; ++rejections_;
        if (!reason_[0]) std::snprintf(reason_, sizeof(reason_), "%s", reason ? reason : "unsupported_external_effect");
    }
    bool eligible() const { return !rejected_; }
    const char* reason() const { return reason_; }
    std::uint64_t rejections() const { return rejections_; }
private:
    bool rejected_ = false;
    char reason_[256]{};
    std::uint64_t rejections_ = 0;
};

class Coordinator {
public:
    bool registerProvider(const Provider& p) {
        const auto d = unsigned(p.domain);
        if (sealed_ || phase_ != Phase::Empty || d >= DomainCount || p.part >= MaxParts ||
            !p.storageBytes || !p.capture || !p.validateRestore || !p.restore || !p.discard ||
            (registered_[d] & (1u << p.part))) return false;
        providers_[d][p.part] = p; registered_[d] |= 1u << p.part; return true;
    }
    // Additional parts can only strengthen the compiled minimum, before the
    // first capture. A partial aggregator cannot reduce a mandatory domain.
    bool requireParts(Domain domain, unsigned mask) {
        const auto d = unsigned(domain);
        if (sealed_ || phase_ != Phase::Empty || d >= DomainCount || mask >= (1u << MaxParts) ||
            (mask & RequiredParts[d]) != RequiredParts[d]) return false;
        required_[d] = mask; return true;
    }
    bool capture(const Frame& frame) {
        if (phase_ != Phase::Empty) return reject(Failure::Busy, "server_checkpoint_already_pinned");
        if (!valid(frame)) return reject(Failure::InvalidIdentity, "server_checkpoint_identity_missing");
        if (registeredDomains() != AllDomains) return reject(Failure::MissingProvider, "server_checkpoint_required_native_provider_missing");
        sealed_ = true; frame_ = frame; phase_ = Phase::Capturing;
        restored_ = validated_ = 0; failure_ = Failure::None; reason_[0] = 0;
        for (unsigned d = 0; d < DomainCount; ++d) for (unsigned p = 0; p < MaxParts; ++p)
            if (registered_[d] & (1u << p)) {
                // Include a rejected provider in cleanup: it may have acquired
                // fixture-only pins before discovering an unsupported state.
                captured_[d] |= 1u << p;
                if (!providers_[d][p].capture(frame_, reason_, sizeof(reason_))) {
                    failure_ = Failure::CaptureRejected; ++failures_; discardImages(); return false;
                }
            }
        phase_ = Phase::Pinned; ++captures_; return true;
    }
    bool validateRestore(const Frame& current) {
        if (phase_ != Phase::Pinned) return reject(Failure::NoCheckpoint, "server_checkpoint_no_pinned_image");
        if (!sameLifetime(frame_, current)) return reject(Failure::LifetimeChanged, "server_checkpoint_original_arena_or_fence_changed");
        phase_ = Phase::Validating; validated_ = restored_ = 0; reason_[0] = 0;
        for (unsigned d = 0; d < DomainCount; ++d) {
            for (unsigned p = 0; p < MaxParts; ++p) if (captured_[d] & (1u << p))
                if (!providers_[d][p].validateRestore(current, reason_, sizeof(reason_))) {
                    phase_ = Phase::Pinned; failure_ = Failure::ValidationRejected; ++failures_; return false;
                }
            validated_ |= 1u << d;
        }
        phase_ = Phase::Pinned; failure_ = Failure::None; ++validations_; return true;
    }
    bool restore(const Frame& current) {
        // Never reuse a previous validation across a pause/fence transition.
        if (!validateRestore(current)) return false;
        phase_ = Phase::Restoring;
        // Restore allocators before pointer-bearing live graphs. No gameplay
        // executes between these commits; registrations cannot alter order.
        constexpr Domain order[DomainCount] = {Domain::ScriptAllocators, Domain::ScriptVM,
            Domain::World, Domain::Graphs, Domain::Animation, Domain::Collision, Domain::Physics,
            Domain::Schedule, Domain::RNG, Domain::Network, Domain::Runtime, Domain::Bots,
            Domain::Admin, Domain::Dvars};
        for (const auto domain : order) {
            const auto d = unsigned(domain);
            for (unsigned p = 0; p < MaxParts; ++p) if (captured_[d] & (1u << p)) providers_[d][p].restore(frame_);
            restored_ |= 1u << d;
        }
        phase_ = Phase::Pinned; failure_ = Failure::None; ++restorations_; return true;
    }
    bool discard() {
        if (phase_ != Phase::Pinned && phase_ != Phase::Empty) return reject(Failure::Busy, "server_checkpoint_operation_in_progress");
        discardImages(); validated_ = restored_ = 0; failure_ = Failure::None; reason_[0] = 0; return true;
    }
    bool reject(Failure failure, const char* reason) {
        failure_ = failure; restored_ = 0; ++failures_;
        std::snprintf(reason_, sizeof(reason_), "%s", reason ? reason : "server_checkpoint_rejected"); return false;
    }
    unsigned registeredDomains() const {
        unsigned mask = 0;
        for (unsigned d = 0; d < DomainCount; ++d)
            if ((registered_[d] & required(d)) == required(d)) mask |= 1u << d;
        return mask;
    }
    unsigned registeredParts(Domain d) const { return unsigned(d) < DomainCount ? registered_[unsigned(d)] : 0; }
    unsigned required(Domain d) const { return unsigned(d) < DomainCount ? required(unsigned(d)) : 0; }
    unsigned capturedDomains() const {
        unsigned mask = 0;
        for (unsigned d = 0; d < DomainCount; ++d)
            if ((captured_[d] & required(d)) == required(d)) mask |= 1u << d;
        return mask;
    }
    unsigned validatedDomains() const { return validated_; }
    unsigned restoredDomains() const { return restored_; }
    bool pinned() const { return phase_ != Phase::Empty; }
    bool restoreComplete() const { return phase_ == Phase::Pinned && restored_ == AllDomains && failure_ == Failure::None; }
    Phase phase() const { return phase_; }
    Failure failure() const { return failure_; }
    const char* reason() const { return reason_; }
    const Frame& frame() const { return frame_; }
    std::uint64_t captures() const { return captures_; }
    std::uint64_t validations() const { return validations_; }
    std::uint64_t restorations() const { return restorations_; }
    std::uint64_t failures() const { return failures_; }
    std::size_t storageBytes() const {
        std::size_t n = 0;
        for (unsigned d = 0; d < DomainCount; ++d) for (unsigned p = 0; p < MaxParts; ++p)
            if (registered_[d] & (1u << p)) n += providers_[d][p].storageBytes;
        return n;
    }
private:
    unsigned required(unsigned d) const { return required_[d] ? required_[d] : RequiredParts[d]; }
    void discardImages() {
        phase_ = Phase::Discarding;
        for (unsigned d = DomainCount; d-- > 0;) for (unsigned p = MaxParts; p-- > 0;)
            if (captured_[d] & (1u << p)) providers_[d][p].discard();
        std::memset(captured_, 0, sizeof(captured_)); phase_ = Phase::Empty;
    }
    Provider providers_[DomainCount][MaxParts]{};
    unsigned registered_[DomainCount]{}, required_[DomainCount]{}, captured_[DomainCount]{};
    Frame frame_{};
    Phase phase_ = Phase::Empty;
    Failure failure_ = Failure::None;
    unsigned validated_ = 0, restored_ = 0;
    bool sealed_ = false;
    char reason_[256]{};
    std::uint64_t captures_ = 0, validations_ = 0, restorations_ = 0, failures_ = 0;
};
} // namespace khz::server_checkpoint
