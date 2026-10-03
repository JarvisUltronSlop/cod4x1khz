#include <snapshot_header_history.hpp>
#include <cstdio>
#include <cstdlib>
#define CHECK(value) do { if (!(value)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #value); return 1; } } while (0)

int main() {
    khz::SnapshotHeaderHistory history;
    khz::WireHeader output{};
    CHECK(!history.read(0, &output));
    history.reset(11);
    CHECK(history.commit(100, {11, 900, 700, 800}));
    CHECK(history.commit(101, {11, 901, 701, 801}));
    CHECK(history.read(100, &output) && output.tick == 900 && output.input == 700 && output.ack == 800);
    CHECK(history.read(101, &output) && output.tick == 901);
    // Late decoder records keep each accepted baseline's original header.
    CHECK(history.commit(99, {11, 899, 699, 799}));
    CHECK(history.read(101, &output) && output.tick == 901);
    CHECK(!history.commit(102, {12, 902, 702, 802}));
    CHECK(!history.commit(102, {11, 0, 702, 802}));
    CHECK(!history.read(102, &output));
    // Ring reuse must not make an expired baseline look like its successor.
    CHECK(history.commit(100 + khz::SnapshotHistory, {11, 1412, 1212, 1312}));
    CHECK(!history.read(100, &output));
    CHECK(history.read(100 + khz::SnapshotHistory, &output) && output.tick == 1412);
    CHECK(!history.commit(100, {11, 900, 700, 800}));
    CHECK(history.read(100 + khz::SnapshotHistory, &output) && output.tick == 1412);
    CHECK(history.read(101, &output) && output.tick == 901);
    CHECK(!history.read(99, &output)); // expiration follows authoritative ms
    history.reset(12);
    CHECK(!history.read(101, &output));
    CHECK(!history.commit(101, {11, 901, 701, 801}));
    CHECK(history.commit(101, {12, 1, 1, 1}));
    CHECK(history.read(101, &output) && output.session == 12 && output.tick == 1);
    history.reset(0);
    CHECK(!history.read(101, &output));
    CHECK(!history.commit(0, {0, 1, 1, 1}));
    CHECK(!history.commit(-1, {12, 1, 1, 1}));
    CHECK(!history.read(0, nullptr));
    std::puts("Snapshot baseline/session header history cases passed");
}
