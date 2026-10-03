#include <client_checkpoint.hpp>
#include <core.hpp>
#include <cassert>
#include <cstdint>
#include <limits>

int main() {
    using namespace khz::checkpoint;
    Frame saved{4, 7, 11, 1250, 10000000, 990};
    assert(valid(saved)); auto current = saved; current.logicalTick += 1000;
    assert(sameLifetime(saved, current));
    current.generation++; assert(!sameLifetime(saved, current));
    current = saved; current.session++; assert(!sameLifetime(saved, current));
    current = saved; current.qpcFrequency++; assert(!sameLifetime(saved, current));
    current = saved; current.fixture++; assert(!sameLifetime(saved, current));
    current = saved; current.session = 0; assert(!valid(current));

    // A captured clock at an arbitrary existing tick is rebased without old
    // wall-clock debt. New absolute deadlines retain the rational 1 kHz phase.
    for (std::uint64_t frequency : {10000000ull, 3579545ull, 1000003ull}) {
        for (std::uint64_t tick : {0ull, 1ull, 997ull, 1250ull, 99999999ull}) {
            const std::uint64_t now = 100000000000000ull;
            khz::DeadlineClock clock; assert(clock.rebase(now, frequency, tick));
            assert(clock.tick() == tick && clock.frequency() == frequency);
            assert(clock.due(now) == 0);
            auto previous = now;
            for (unsigned i = 0; i < 4096; ++i) {
                const auto deadline = clock.deadline(); assert(deadline > previous);
                assert(clock.due(deadline - 1) == 0 && clock.due(deadline) == 1);
                previous = deadline; clock.advance();
            }
            assert(clock.tick() == tick + 4096);
        }
    }
    khz::DeadlineClock clock;
    assert(!clock.rebase(1, 10000000, 1000));
    assert(!clock.rebase(1000, 0, 0));
    assert(!clock.rebase(UINT64_MAX, UINT64_MAX, UINT64_MAX));

    // Replaying every bounded receipt on a fresh QPC origin preserves order
    // and the exact ceil conversion at a non-divisible hardware frequency.
    std::uint64_t last = 0;
    for (unsigned tick = 0; tick < 4096; ++tick) {
        for (unsigned subtick : {0u, 1u, 500u, 999u}) {
            std::uint64_t deadline = 0;
            assert(receiptDeadline(80000000, 3579545, tick, subtick, &deadline));
            const auto numerator = (std::uint64_t(tick) * 1000 + subtick) * 3579545;
            assert(deadline == 80000000 + numerator / 1000000 + (numerator % 1000000 != 0));
            assert(deadline >= last); last = deadline;
        }
    }
    std::uint64_t unchanged = 123;
    assert(!receiptDeadline(10, 0, 0, 0, &unchanged) && unchanged == 123);
    assert(!receiptDeadline(10, 3579545, 0, 1000, &unchanged) && unchanged == 123);
    assert(!receiptDeadline(UINT64_MAX, 3579545, 1, 0, &unchanged) && unchanged == 123);
    assert(!receiptDeadline(0, UINT64_MAX, UINT64_MAX, 999, &unchanged) && unchanged == 123);
}
