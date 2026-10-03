#include <input_timestamp.hpp>
#include <core.hpp>
#include <array>
#include <cstdio>
#include <cstdlib>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); std::exit(1); } } while (0)
namespace {
struct Command { unsigned buttons; int serverTime; };
bool admitted(const Command& command, int authoritativeTime) {
    return command.serverTime <= authoritativeTime + 200 && command.serverTime >= authoritativeTime - 2000;
}
}
int main() {
    // Reproduce the measured audio-reload boundary: the original next command
    // was rejected, even though its sequence was fresh and the player remained
    // ACTIVE. A newly accepted snapshot repairs only its admission timestamp.
    int stamp = 28068;
    const int authoritative = 30316;
    CHECK(!admitted({0, stamp + 1}, authoritative));
    khz::SequenceWindow<2048> received;
    CHECK(received.accept(7812));
    std::array<Command, 2048> history{};
    history[7813 & 2047] = {1, 28067};
    history[7814 & 2047] = {0, 28068};
    const auto retained = history;
    const unsigned sequence = 7894;
    stamp = khz::alignInputTimestamp(stamp, authoritative);
    Command fresh{0, ++stamp};
    CHECK(admitted(fresh, authoritative) && received.accept(sequence));
    CHECK(received.highest() == sequence);
    for (unsigned i = 0; i != history.size(); ++i)
        CHECK(history[i].serverTime == retained[i].serverTime && history[i].buttons == retained[i].buttons);
    CHECK(!received.accept(sequence)); // timestamp alignment cannot defeat duplication checks
    CHECK(!admitted(retained[7813 & 2047], authoritative)); // old commands remain subject to admission

    // Held/pressed input semantics survive resynchronization. No synthetic
    // button edges or acknowledgement/sequence resets are introduced.
    khz::InputLatch<Command> latch;
    latch.receive({1, fresh.serverTime}, sequence);
    CHECK(latch.consume().buttons == 1);
    stamp = khz::alignInputTimestamp(stamp, authoritative - 500); // delayed snapshot never rewinds
    CHECK(stamp == fresh.serverTime && latch.sequence() == sequence);
    latch.receive({0, ++stamp}, sequence + 1);
    CHECK(latch.consume().buttons == 0 && latch.consume().buttons == 0);

    // Repeated snapshots, moderate jitter and frontend stalls of varied length
    // preserve one fresh native input per actual gameplay tick.
    int nativeTime = 50000, localTime = nativeTime;
    unsigned commands = 0;
    khz::SequenceWindow<2048> window;
    for (unsigned tick = 0; tick != 4000; ++tick) {
        nativeTime += tick == 1000 ? 3100 : tick == 2000 ? 2250 : tick == 3000 ? 850 : 1;
        if (tick % 4 == 0) localTime = khz::alignInputTimestamp(localTime, nativeTime);
        else localTime = khz::alignInputTimestamp(localTime, nativeTime - 7);
        CHECK(admitted({0, ++localTime}, nativeTime));
        CHECK(window.accept(++commands));
    }
    CHECK(commands == 4000 && window.highest() == 4000);
    std::puts("input timestamp: restart admission, monotonic accepted snapshots, preserved sequences/history/edges pass");
}
