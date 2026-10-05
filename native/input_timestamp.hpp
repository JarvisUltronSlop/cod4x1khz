#pragma once
#include <cstdint>

namespace khz {
// Native usercmd timestamps validate admission; protocol input sequences and
// prediction's fixed one-millisecond replay remain independent of this clock.
// Call only after accepting a complete snapshot in the current session.
inline int alignInputTimestamp(int previous, int authoritativeCommandTime) {
    return authoritativeCommandTime > previous ? authoritativeCommandTime : previous;
}
// Catch-up ticks can also push the admission clock ahead of the server. Only
// a new authoritative time may rebase it; delayed snapshots cannot rewind it.
// Leave 100 ms of margin inside the server's +200 ms admission bound. Sequence
// numbers and saved commands are independent and must never be rebased here.
inline int alignInputTimestamp(int previous, int authoritativeCommandTime, int& newestAuthoritativeTime) {
    if (authoritativeCommandTime < newestAuthoritativeTime) return previous;
    newestAuthoritativeTime = authoritativeCommandTime;
    if (std::int64_t(previous) - authoritativeCommandTime > 100) return authoritativeCommandTime;
    return alignInputTimestamp(previous, authoritativeCommandTime);
}
} // namespace khz
