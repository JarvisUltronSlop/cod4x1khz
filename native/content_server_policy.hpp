#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

// Allocation-free admission and request storage. Native FS lookup, hashing and
// reliable-command construction happen only in the service/worker phases.
namespace khz::content::server {
inline constexpr unsigned Slots = 16;
struct Admission {
    unsigned slot = Slots;
    std::uint64_t lifecycle = 0, session = 0, mapGeneration = 0;
    std::array<std::uint8_t, 32> fingerprint{};
    bool connected = false, bot = false;
};
inline bool Accepted(const Admission& value) {
    if (value.slot >= Slots || !value.connected || value.bot || !value.lifecycle || !value.session || !value.mapGeneration) return false;
    for (auto byte : value.fingerprint) if (byte) return true;
    return false;
}
inline bool SameConnection(const Admission& captured, const Admission& current) {
    return Accepted(captured) && Accepted(current) && captured.slot == current.slot
        && captured.lifecycle == current.lifecycle && captured.session == current.session
        && captured.mapGeneration == current.mapGeneration && captured.fingerprint == current.fingerprint;
}
inline bool PrefixComponent(std::string_view value, std::string_view prefix) {
    return !prefix.empty() && value.size() > prefix.size() && value.substr(0, prefix.size()) == prefix && value[prefix.size()] == '/';
}
// Syntax is validated by the shared codec. This is an additional active-map/mod
// scope check; callers must still require exact FS/DB advertisement membership.
inline bool AllowedScope(std::string_view relative, std::string_view activeMod, std::string_view activeMap) {
    if (relative.substr(0, 5) == "main/") {
        const auto leaf = relative.substr(5);
        const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? char(c + 32) : c; };
        // Nonstock packages in main are a native advertised-IWD path too.
        // Shared syntax, exact referenced membership and confined resolution
        // remain mandatory; this scope check alone never authorizes a file.
        return leaf.size() > 4 && leaf.find('/') == std::string_view::npos && leaf.substr(leaf.size() - 4) == ".iwd" &&
            !(leaf.size() >= 3 && lower(leaf[0]) == 'i' && lower(leaf[1]) == 'w' && leaf[2] == '_');
    }
    if (activeMod.size() > 5 && activeMod.substr(0, 5) == "mods/" && PrefixComponent(relative, activeMod)) return true;
    if (activeMap.empty() || relative.substr(0, 9) != "usermaps/") return false;
    relative.remove_prefix(9);
    if (!PrefixComponent(relative, activeMap)) return false;
    relative.remove_prefix(activeMap.size() + 1);
    // Only this map's IWD, main fastfile and loading fastfile are downloadable.
    if ((relative.size() == activeMap.size() + 4 || relative.size() == activeMap.size() + 3) && relative.substr(0, activeMap.size()) == activeMap)
        return relative.substr(activeMap.size()) == ".iwd" || relative.substr(activeMap.size()) == ".ff";
    return relative.size() == activeMap.size() + 8 && relative.substr(0, activeMap.size()) == activeMap
        && relative.substr(activeMap.size()) == "_load.ff";
}
struct Pending {
    Admission admission{};
    std::uint64_t id = 0;
    char relative[240]{};
};
enum class Enqueue { Accepted, Invalid, Busy, Duplicate, Limited };
class Requests {
    struct Slot { Pending pending{}; std::uint64_t highest = 0, lifecycle = 0, nextAllowed = 0; bool waiting = false, inFlight = false; };
    std::array<Slot, Slots> slots_{};
    unsigned next_ = 0;
public:
    Enqueue Put(const Pending& value, std::uint64_t nowMs) {
        if (!Accepted(value.admission) || !value.id || !value.relative[0] || !std::memchr(value.relative, 0, sizeof(value.relative))) return Enqueue::Invalid;
        auto& state = slots_[value.admission.slot];
        if (state.lifecycle != value.admission.lifecycle || (state.highest && !SameConnection(state.pending.admission, value.admission))) { state = {}; state.lifecycle = value.admission.lifecycle; }
        if (value.id <= state.highest) return Enqueue::Duplicate;
        if (state.waiting || state.inFlight) return Enqueue::Busy;
        if (nowMs < state.nextAllowed) return Enqueue::Limited;
        state.pending = value; state.highest = value.id; state.nextAllowed = nowMs + 100; state.waiting = true;
        return Enqueue::Accepted;
    }
    bool Take(Pending* value) {
        if (!value) return false;
        for (unsigned n = 0; n < Slots; ++n) {
            const unsigned index = (next_ + n) % Slots; auto& state = slots_[index];
            if (!state.waiting) continue;
            *value = state.pending; state.waiting = false; state.inFlight = true; next_ = (index + 1) % Slots; return true;
        }
        return false;
    }
    bool Finish(const Pending& value) {
        if (value.admission.slot >= Slots) return false;
        auto& state = slots_[value.admission.slot];
        if (!state.inFlight || !SameConnection(state.pending.admission, value.admission) || state.pending.id != value.id) return false;
        state.inFlight = false; return true;
    }
    void Reset() { slots_ = {}; next_ = 0; }
};
}
