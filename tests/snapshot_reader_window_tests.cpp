#include "snapshot_reader_window.hpp"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); std::exit(1); } } while (0)
int main() {
    const auto startup = khz::snapshotReaderWindow(1136,37,512);
    CHECK(startup.valid && startup.processed == 624 && startup.expired == 587);
    CHECK(1136 - (startup.processed + 1) == 511);
    CHECK(khz::snapshotReaderWindow(1136,startup.processed,512).expired == 0);
    for (int distance = 0; distance < 5000; ++distance) {
        const auto window = khz::snapshotReaderWindow(10000,10000-distance,512);
        CHECK(window.valid && window.processed >= 10000-distance && window.processed <= 10000);
        CHECK(window.expired == (distance > 512 ? unsigned(distance-512) : 0));
        // Every skipped id fails the native retention rule; every retained
        // unread id remains in the same order. Nothing usable is coalesced.
        for (int message = 10000-distance+1; message <= window.processed; ++message) CHECK(10000-message >= 512);
        for (int message = window.processed+1; message <= 10000; ++message) CHECK(10000-message < 512);
    }
    const auto reset = khz::snapshotReaderWindow(7,1000,512); CHECK(!reset.valid && reset.processed == 1000 && !reset.expired);
    CHECK(!khz::snapshotReaderWindow(7,0,0).valid);
    constexpr int maximum = (std::numeric_limits<int>::max)(), minimum = (std::numeric_limits<int>::min)();
    const auto extremes = khz::snapshotReaderWindow(maximum,minimum,512);
    CHECK(extremes.valid && extremes.processed == maximum-512 && extremes.expired == 4294966783ULL);
    const auto negative = khz::snapshotReaderWindow(minimum+700,minimum+1,512);
    CHECK(negative.valid && negative.processed == minimum+188 && negative.expired == 187);
    std::puts("snapshot expired-prefix retention and order passed");
}
