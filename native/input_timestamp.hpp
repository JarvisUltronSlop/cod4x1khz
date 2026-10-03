#pragma once

namespace khz {
// Native usercmd timestamps validate admission; protocol input sequences and
// prediction's fixed one-millisecond replay remain independent of this clock.
// Call only after accepting a complete snapshot in the current session.
inline int alignInputTimestamp(int previous, int authoritativeCommandTime) {
    return authoritativeCommandTime > previous ? authoritativeCommandTime : previous;
}
} // namespace khz
