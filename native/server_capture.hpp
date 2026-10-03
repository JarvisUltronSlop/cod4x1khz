#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <type_traits>

namespace khz::server_capture {
constexpr unsigned MaxSlots = 16, MaxTicks = 4096;
constexpr unsigned MaxConsumedCommands = 2048;
constexpr std::uint64_t SlotMask = (1ull << MaxSlots) - 1;
enum class Source : unsigned { Held = 0, Queued = 1, NativeBot = 2 };
enum class Failure : unsigned {
    None, TickAlreadyOpen, TickNotOpen, TickContinuity, TimeContinuity,
    Lifecycle, ActiveSlotRange, InactiveSlot, DuplicateSlot, MissingSlot,
    SlotGeneration, InvalidCommand, InvalidInputMetadata, Overflow,
    IncompleteTick, EndedEarly
};
struct Command {
    std::int32_t serverTime, buttons, angles[3];
    std::uint8_t weapon, offHandIndex;
    std::int8_t forwardmove, rightmove;
    float meleeChargeYaw;
    std::uint8_t meleeChargeDist;
    std::int8_t selectedLocation[2];
};
struct SlotRecord {
    std::uint64_t generation, inputSequenceBefore, inputSequenceAfter;
    std::uint64_t appliedSequence, receivedSequence, viewTick;
    unsigned slot, consumedCommands;
    Source source;
    std::int32_t entityUseCount, lastConnectTime, lastSpawnTime, serverId;
    std::uint16_t scriptId;
    bool inputAdvanced;
    Command command;
};
struct Request {
    std::uint64_t fixture, session;
    unsigned ticks;
    std::int32_t serverId;
};
struct TickRecord {
    std::uint64_t fixture, session, tick, ordinal;
    std::int32_t serverTime, serverId;
    std::uint32_t activeMaskBefore, activeMaskAfter, recordedMask;
    std::uint32_t restoreConditions; // Always zero: a tape is not a restore.
    Failure failure;
    bool complete;
    SlotRecord slots[MaxSlots];
};
struct Status {
    Request request;
    unsigned recorded, drained, invalid, rejected, queued;
    std::uint64_t firstTick, lastTick;
    Failure failure;
    bool active, tickOpen, finished, complete, exportComplete;
};
static_assert(std::is_trivially_copyable<TickRecord>::value, "fixed native observation records");

// The dedicated game owner is the sole caller. Storage is an external fixed
// arena, never a large aggregate constructed on the CRT or gameplay stack.
// Completed records are immutable until a new, quiescent begin after draining.
template<unsigned Capacity> class Tape {
    static_assert(Capacity && Capacity <= MaxTicks);
public:
    explicit Tape(TickRecord* storage) : storage_(storage) {}
    bool begin(const Request& request) {
        if (!storage_ || !request.fixture || !request.session || !request.ticks ||
            request.ticks > Capacity || active_ || open_ || drained_ != recorded_) return false;
        request_ = request; recorded_ = drained_ = invalid_ = rejected_ = 0;
        firstTick_ = lastTick_ = 0; lastTime_ = 0; failure_ = Failure::None;
        finished_ = false; active_ = true; return true;
    }
    bool beginTick(std::uint64_t session, std::uint64_t tick, std::int32_t time, std::uint64_t activeMask) {
        if (open_) return reject(Failure::TickAlreadyOpen);
        if (!active_) return false;
        if (recorded_ >= Capacity) return reject(Failure::Overflow);
        if (session != request_.session || !tick) return reject(Failure::Lifecycle);
        if (activeMask & ~SlotMask) return reject(Failure::ActiveSlotRange);
        if (recorded_ && (lastTick_ == UINT64_MAX || tick != lastTick_ + 1))
            return reject(Failure::TickContinuity);
        if (recorded_ && (lastTime_ == (std::numeric_limits<std::int32_t>::max)() || time != lastTime_ + 1))
            return reject(Failure::TimeContinuity);
        auto& record = storage_[recorded_];
        // <=2 KiB per tick, including the unused fixed slot tail. This gives
        // consumers deterministic inactive slots without allocation or logging.
        std::memset(&record, 0, sizeof(record));
        record.fixture = request_.fixture; record.session = session;
        record.tick = tick; record.ordinal = recorded_ + 1; record.serverTime = time;
        record.serverId = request_.serverId; record.activeMaskBefore = static_cast<std::uint32_t>(activeMask);
        open_ = true; if (!recorded_) firstTick_ = tick; return true;
    }
    bool record(const SlotRecord& slot) {
        if (!open_) return active_ ? reject(Failure::TickNotOpen) : false;
        if (failure_ != Failure::None) return false;
        auto& tick = storage_[recorded_];
        if (slot.slot >= MaxSlots) return reject(Failure::ActiveSlotRange);
        const auto bit = 1u << slot.slot;
        if (!(tick.activeMaskBefore & bit)) return reject(Failure::InactiveSlot);
        if (tick.recordedMask & bit) return reject(Failure::DuplicateSlot);
        if (!slot.generation) return reject(Failure::SlotGeneration);
        if (slot.command.serverTime != tick.serverTime || !std::isfinite(slot.command.meleeChargeYaw))
            return reject(Failure::InvalidCommand);
        if (slot.viewTick > tick.tick || slot.inputSequenceAfter < slot.inputSequenceBefore ||
            slot.inputAdvanced != (slot.inputSequenceAfter > slot.inputSequenceBefore) || slot.consumedCommands > MaxConsumedCommands)
            return reject(Failure::InvalidInputMetadata);
        if (slot.source == Source::NativeBot) {
            if (slot.consumedCommands || slot.inputSequenceBefore != tick.tick || slot.inputSequenceAfter != tick.tick)
                return reject(Failure::InvalidInputMetadata);
        } else if (slot.source == Source::Held || slot.source == Source::Queued) {
            if (slot.appliedSequence > slot.receivedSequence || slot.inputSequenceAfter > slot.receivedSequence ||
                ((slot.source == Source::Held) != (slot.consumedCommands == 0)) ||
                (slot.source == Source::Held && slot.inputSequenceAfter != slot.inputSequenceBefore))
                return reject(Failure::InvalidInputMetadata);
        } else return reject(Failure::InvalidInputMetadata);
        tick.slots[slot.slot] = slot; tick.recordedMask |= bit; return true;
    }
    bool endTick(std::uint64_t session, std::uint64_t tick, std::int32_t time,
        std::uint64_t activeMask, const std::uint64_t (&generations)[MaxSlots]) {
        if (!open_) return active_ ? reject(Failure::TickNotOpen) : false;
        auto& record = storage_[recorded_];
        record.activeMaskAfter = static_cast<std::uint32_t>(activeMask);
        if (session != record.session) reject(Failure::Lifecycle);
        else if (tick != record.tick) reject(Failure::TickContinuity);
        else if (time != record.serverTime) reject(Failure::TimeContinuity);
        else if (activeMask & ~SlotMask) reject(Failure::ActiveSlotRange);
        else if (activeMask != record.activeMaskBefore) reject(Failure::Lifecycle);
        else if (record.recordedMask != record.activeMaskBefore) reject(Failure::MissingSlot);
        else for (unsigned slot = 0; slot < MaxSlots; ++slot)
            if ((record.recordedMask & (1u << slot)) && generations[slot] != record.slots[slot].generation)
                { reject(Failure::SlotGeneration); break; }
        return commit();
    }
    void invalidateLifecycle() {
        if (!active_ && !open_) return;
        reject(Failure::Lifecycle);
        if (open_) commit();
    }
    void end() {
        if (!request_.fixture) return;
        if (open_) { reject(Failure::IncompleteTick); commit(); }
        if (recorded_ != request_.ticks) reject(Failure::EndedEarly);
        active_ = false; finished_ = true;
    }
    bool pop(TickRecord& record) {
        if (drained_ == recorded_) return false;
        record = storage_[drained_++]; return true;
    }
    const TickRecord* find(unsigned ordinal) const {
        return ordinal && ordinal <= recorded_ ? &storage_[ordinal - 1] : nullptr;
    }
    bool active() const { return active_; }
    bool tickOpen() const { return open_; }
    bool reject(Failure reason) {
        ++rejected_; if (failure_ == Failure::None) failure_ = reason;
        if (open_) storage_[recorded_].failure = failure_;
        active_ = false; finished_ = true; return false;
    }
    Status status() const {
        Status result{}; result.request = request_; result.recorded = recorded_; result.drained = drained_;
        result.invalid = invalid_; result.rejected = rejected_; result.queued = recorded_ - drained_;
        result.firstTick = firstTick_; result.lastTick = lastTick_; result.failure = failure_;
        result.active = active_; result.tickOpen = open_; result.finished = finished_;
        result.complete = request_.fixture && finished_ && !active_ && !open_ &&
            failure_ == Failure::None && recorded_ == request_.ticks;
        result.exportComplete = result.complete && drained_ == recorded_; return result;
    }
private:
    bool commit() {
        auto& record = storage_[recorded_]; record.failure = failure_;
        record.complete = failure_ == Failure::None;
        if (!record.complete) ++invalid_;
        lastTick_ = record.tick; lastTime_ = record.serverTime; ++recorded_; open_ = false;
        if (recorded_ == request_.ticks || failure_ != Failure::None) { active_ = false; finished_ = true; }
        return record.complete;
    }
    TickRecord* storage_;
    Request request_{};
    unsigned recorded_ = 0, drained_ = 0, invalid_ = 0, rejected_ = 0;
    std::uint64_t firstTick_ = 0, lastTick_ = 0;
    std::int32_t lastTime_ = 0;
    Failure failure_ = Failure::None;
    bool active_ = false, open_ = false, finished_ = false;
};
static_assert(sizeof(TickRecord) <= 2048, "bounded clearing/copying per 1 ms tick");
} // namespace khz::server_capture
