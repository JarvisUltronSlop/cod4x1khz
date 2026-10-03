#include <server_capture.hpp>
#include <cstdio>
#include <limits>

using namespace khz::server_capture;
namespace {
TickRecord storage[MaxTicks]; // Same raw arena shape as the native observer.
unsigned cases, failures;
void check(bool value) { ++cases; if (!value) ++failures; }
SlotRecord slot(unsigned index, unsigned tick, int time, Source source = Source::Held) {
    SlotRecord record{}; record.slot = index; record.generation = 100 + index;
    record.command.serverTime = time; record.viewTick = tick;
    record.source = source;
    if (source == Source::NativeBot) record.inputSequenceBefore = record.inputSequenceAfter = tick;
    else { record.inputSequenceBefore = record.inputSequenceAfter = record.appliedSequence = record.receivedSequence = 5; }
    if (source == Source::Queued) { record.consumedCommands = 2; record.inputSequenceAfter = record.appliedSequence = record.receivedSequence = 7; record.inputAdvanced = true; }
    record.command.buttons = static_cast<int>(1u << (index % 31));
    record.command.angles[0] = static_cast<int>(index * 100); record.command.angles[1] = -static_cast<int>(index);
    record.command.angles[2] = 65535; record.command.weapon = static_cast<unsigned char>(index + 1);
    record.command.offHandIndex = 9; record.command.forwardmove = -127; record.command.rightmove = 127;
    record.command.meleeChargeYaw = -0.75f; record.command.meleeChargeDist = 255;
    record.command.selectedLocation[0] = -128; record.command.selectedLocation[1] = 127;
    record.entityUseCount = 12; record.scriptId = 27; record.lastSpawnTime = 4;
    return record;
}
bool close(Tape<MaxTicks>& tape, std::uint64_t tick, int time, std::uint64_t mask, unsigned altered = MaxSlots) {
    std::uint64_t generations[MaxSlots]{};
    for (unsigned index = 0; index < MaxSlots; ++index) generations[index] = 100 + index + (index == altered);
    return tape.endTick(7, tick, time, mask, generations);
}
void consume(Tape<MaxTicks>& tape) { TickRecord record; while (tape.pop(record)) {} }
}
int main() {
    Tape<MaxTicks> tape(storage);
    check(!tape.begin({0, 7, 1, 1})); check(!tape.begin({1, 0, 1, 1}));
    check(!tape.begin({1, 7, 0, 1})); check(!tape.begin({1, 7, MaxTicks + 1, 1}));
    check(tape.begin({1, 7, MaxTicks, 32})); check(!tape.begin({2, 7, 1, 32}));
    // Fill the whole bounded arena with all16slots, including held reuse,
    // queued input and native bots. No consumer/drainer is needed to avoid loss.
    for (unsigned tick = 1; tick <= MaxTicks; ++tick) {
        const int time = 900 + static_cast<int>(tick);
        check(tape.beginTick(7, tick, time, SlotMask));
        for (unsigned index = 0; index < MaxSlots; ++index)
            check(tape.record(slot(index, tick, time, static_cast<Source>(index % 3))));
        check(close(tape, tick, time, SlotMask));
    }
    auto status = tape.status(); check(status.complete && !status.active && status.recorded == MaxTicks);
    check(!status.exportComplete && status.queued == MaxTicks);
    check(!tape.begin({2, 7, 1, 32}));
    TickRecord output;
    for (unsigned index = 1; index <= MaxTicks; ++index) {
        check(tape.pop(output)); check(output.complete && output.ordinal == index && output.tick == index);
        check(output.activeMaskBefore == SlotMask && output.recordedMask == SlotMask && output.activeMaskAfter == SlotMask);
        check(output.restoreConditions == 0 && output.slots[4].command.forwardmove == -127 &&
            output.slots[4].command.rightmove == 127 && output.slots[4].command.meleeChargeYaw == -0.75f &&
            output.slots[4].command.selectedLocation[0] == -128 && output.slots[4].command.selectedLocation[1] == 127);
        check(output.slots[5].source == Source::NativeBot && output.slots[5].inputSequenceAfter == index);
    }
    check(!tape.pop(output)); check(tape.status().exportComplete && tape.find(MaxTicks));
    check(!tape.find(0) && !tape.find(MaxTicks + 1));

    // A late older command can pulse buttons without advancing the held input
    // sequence; it must be queued, rather than falsely classified as held/new.
    check(tape.begin({2, 7, 1, 32})); check(tape.beginTick(7, 91, 409, 1));
    auto older = slot(0, 91, 409, Source::Queued);
    older.inputSequenceBefore = older.inputSequenceAfter = older.appliedSequence = older.receivedSequence = 20;
    older.inputAdvanced = false; check(tape.record(older)); check(close(tape, 91, 409, 1));
    check(tape.find(1)->slots[0].source == Source::Queued && !tape.find(1)->slots[0].inputAdvanced);
    consume(tape);

    const auto runInvalid = [&](Failure expected, const auto& operation, std::uint64_t mask = 1) {
        check(tape.begin({3, 7, 1, 32})); check(tape.beginTick(7, 100, 500, mask));
        operation(); close(tape, 100, 500, mask);
        check(!tape.status().complete && tape.status().finished && tape.status().failure == expected);
        check(tape.find(1) && !tape.find(1)->complete && tape.find(1)->restoreConditions == 0); consume(tape);
    };
    runInvalid(Failure::MissingSlot, [&] {});
    runInvalid(Failure::DuplicateSlot, [&] { check(tape.record(slot(0, 100, 500))); check(!tape.record(slot(0, 100, 500))); });
    runInvalid(Failure::InactiveSlot, [&] { check(!tape.record(slot(1, 100, 500))); });
    runInvalid(Failure::ActiveSlotRange, [&] { check(!tape.record(slot(16, 100, 500))); });
    runInvalid(Failure::SlotGeneration, [&] { auto value = slot(0, 100, 500); value.generation = 0; check(!tape.record(value)); });
    runInvalid(Failure::InvalidCommand, [&] { auto value = slot(0, 100, 500); value.command.meleeChargeYaw = (std::numeric_limits<float>::quiet_NaN)(); check(!tape.record(value)); });
    runInvalid(Failure::InvalidCommand, [&] { auto value = slot(0, 100, 501); check(!tape.record(value)); });
    runInvalid(Failure::InvalidInputMetadata, [&] { auto value = slot(0, 100, 500); value.viewTick = 101; check(!tape.record(value)); });
    runInvalid(Failure::InvalidInputMetadata, [&] { auto value = slot(0, 100, 500); value.consumedCommands = 1; check(!tape.record(value)); });
    runInvalid(Failure::InvalidInputMetadata, [&] { auto value = slot(0, 100, 500, Source::Queued); value.consumedCommands = MaxConsumedCommands + 1; check(!tape.record(value)); });
    runInvalid(Failure::InvalidInputMetadata, [&] { auto value = slot(0, 100, 500); value.appliedSequence = 99; check(!tape.record(value)); });
    runInvalid(Failure::TickAlreadyOpen, [&] { check(!tape.beginTick(7, 100, 500, 1)); });

    check(tape.begin({4, 7, 1, 32})); check(tape.beginTick(7, 100, 500, 1)); check(tape.record(slot(0, 100, 500)));
    check(!close(tape, 100, 500, 1, 0)); check(tape.status().failure == Failure::SlotGeneration); consume(tape);
    check(tape.begin({5, 7, 1, 32})); check(tape.beginTick(7, 100, 500, 1)); check(tape.record(slot(0, 100, 500)));
    check(!close(tape, 100, 500, 3)); check(tape.status().failure == Failure::Lifecycle); consume(tape);
    check(tape.begin({6, 7, 1, 32})); check(tape.beginTick(7, 100, 500, 1)); tape.end();
    check(tape.status().failure == Failure::IncompleteTick && !tape.status().complete); consume(tape);
    check(tape.begin({7, 7, 2, 32})); check(tape.beginTick(7, 100, 500, 0)); check(close(tape, 100, 500, 0));
    check(!tape.beginTick(7, 102, 501, 0)); check(tape.status().failure == Failure::TickContinuity); consume(tape);
    check(tape.begin({8, 7, 2, 32})); check(tape.beginTick(7, 100, 500, 0)); check(close(tape, 100, 500, 0));
    check(!tape.beginTick(7, 101, 502, 0)); check(tape.status().failure == Failure::TimeContinuity); consume(tape);
    check(tape.begin({9, 7, 1, 32})); check(!tape.beginTick(8, 100, 500, 0)); check(tape.status().failure == Failure::Lifecycle);
    check(tape.begin({10, 7, 1, 32})); check(!tape.beginTick(7, 100, 500, 1ull << 16)); check(tape.status().failure == Failure::ActiveSlotRange);
    check(tape.begin({11, 7, 1, 32})); tape.end(); check(tape.status().failure == Failure::EndedEarly);
    check(tape.begin({12, 7, 1, 32})); check(tape.beginTick(7, 100, 500, 0)); tape.invalidateLifecycle();
    check(tape.status().failure == Failure::Lifecycle && tape.find(1) && !tape.find(1)->complete); consume(tape);
    std::printf("KHZ_SERVER_CAPTURE_TEST {\"cases\":%u,\"failures\":%u,\"ticks_capacity\":%u,\"slots_capacity\":%u,\"record_bytes\":%zu,\"storage_bytes\":%zu,\"verified_restore_conditions\":0,\"scope\":\"bounded actual command observation policy; native hooks and full-world restoration require separate evidence\"}\n",
        cases, failures, MaxTicks, MaxSlots, sizeof(TickRecord), sizeof(storage));
    return failures ? 1 : 0;
}
