#pragma once
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cmath>
#include <limits>
#include <type_traits>

namespace khz {
using Tick = std::uint64_t;
inline constexpr unsigned TickRate = 1000;
inline constexpr unsigned MaxCatchup = 8;
inline constexpr unsigned InputHistory = 2048;
inline constexpr unsigned SnapshotHistory = 512;
inline constexpr unsigned ParsedEntities = 262144;
inline constexpr unsigned ParsedClients = 32768;
inline constexpr unsigned RewindHistory = 256;
inline constexpr int Protocol = 10002;
inline constexpr std::uint32_t WireMagic = 0x315A484B; // KHZ1, little endian

inline Tick waitTicks(double seconds) {
    const auto ticks = static_cast<Tick>(std::ceil(seconds * 1000.0));
    return ticks ? ticks : 1;
}
inline Tick waitTicks(float seconds) {
    // Multiply in the script value's precision. Decimal millisecond waits such
    // as 0.05f round to 50 ms, rather than 51 after promoting float noise.
    const auto ticks = static_cast<Tick>(std::ceil(double(seconds * 1000.0f)));
    return ticks ? ticks : 1;
}

template<std::size_t Capacity> class SequenceWindow {
    static_assert(Capacity && (Capacity & (Capacity - 1)) == 0);
public:
    bool accept(std::uint64_t sequence) {
        if (!sequence || (sequence <= highest_ && highest_ - sequence >= Capacity)) return false;
        auto& tag = tags_[sequence & (Capacity - 1)];
        if (tag == sequence) return false;
        tag = sequence;
        if (sequence > highest_) highest_ = sequence;
        return true;
    }
    void reset() { tags_.fill(0); highest_ = 0; }
    std::uint64_t highest() const { return highest_; }
private:
    std::array<std::uint64_t, Capacity> tags_{};
    std::uint64_t highest_ = 0;
};

// The newest command owns held input. An earlier press arriving after its
// release produces one tick's pulse, without restoring old movement or angles.
template<class Command> class InputLatch {
public:
    void reset(const Command& initial = {}) { held_ = initial; sequence_ = 0; pressed_ = 0; }
    void receive(const Command& command, std::uint64_t sequence) {
        pressed_ |= command.buttons & ~held_.buttons;
        if (sequence > sequence_) { held_ = command; sequence_ = sequence; }
    }
    Command consume() {
        auto command = held_;
        command.buttons |= pressed_;
        pressed_ = 0;
        return command;
    }
    std::uint64_t sequence() const { return sequence_; }
private:
    Command held_{};
    std::uint64_t sequence_ = 0;
    unsigned pressed_ = 0;
};

template<class Value, std::size_t Capacity> class TickHistory {
    static_assert(Capacity && (Capacity & (Capacity - 1)) == 0);
public:
    TickHistory() { reset(); }
    void reset() { tags_.fill((std::numeric_limits<Tick>::max)()); }
    Value& begin(Tick tick) { const auto slot = tick & (Capacity - 1); tags_[slot] = tick; return values_[slot]; }
    const Value* find(Tick tick) const { const auto slot = tick & (Capacity - 1); return tags_[slot] == tick ? &values_[slot] : nullptr; }
private:
    std::array<Value, Capacity> values_{};
    std::array<Tick, Capacity> tags_{};
};

// Absolute rational deadlines avoid accumulating error when QPC frequency
// is not divisible by 1000. A scheduler never discards simulation debt.
class DeadlineClock {
public:
    void reset(std::uint64_t now, std::uint64_t frequency) {
        frequency_ = frequency; epoch_ = now; tick_ = 0;
    }
    // Same-process replay retains the logical tick but gets a fresh wall-clock
    // epoch. Preserve the rational phase and reject underflow/overflow instead
    // of creating artificial catch-up debt from saved absolute QPC deadlines.
    bool rebase(std::uint64_t now, std::uint64_t frequency, Tick tick) {
        if (frequency < TickRate) return false;
        const auto seconds = tick / TickRate, remainder = tick % TickRate;
        const auto maximum = (std::numeric_limits<std::uint64_t>::max)();
        if (seconds > maximum / frequency || (remainder && frequency > maximum / remainder)) return false;
        const auto whole = seconds * frequency, partial = remainder * frequency / TickRate;
        if (whole > maximum - partial || now < whole + partial || tick == maximum) return false;
        const auto next = tick + 1, nextSeconds = next / TickRate, nextRemainder = next % TickRate;
        if (nextSeconds > maximum / frequency || (nextRemainder && frequency > (maximum - TickRate + 1) / nextRemainder)) return false;
        const auto nextWhole = nextSeconds * frequency;
        const auto nextPartial = (nextRemainder * frequency + TickRate - 1) / TickRate;
        const auto epoch = now - whole - partial;
        if (nextWhole > maximum - nextPartial || epoch > maximum - nextWhole - nextPartial) return false;
        frequency_ = frequency; epoch_ = epoch; tick_ = tick;
        return true;
    }
    std::uint64_t deadline() const {
        const auto next = tick_ + 1;
        return epoch_ + (next / TickRate) * frequency_
            + ((next % TickRate) * frequency_ + TickRate - 1) / TickRate;
    }
    Tick debt(std::uint64_t now) const {
        if (!frequency_ || now < deadline()) return 0;
        const auto elapsed = now - epoch_;
        const Tick available = (elapsed / frequency_) * TickRate
            + ((elapsed % frequency_) * TickRate) / frequency_;
        if (available <= tick_) return 0;
        return available - tick_;
    }
    unsigned due(std::uint64_t now) const { const auto value = debt(now); return static_cast<unsigned>(value > MaxCatchup ? MaxCatchup : value); }
    void advance() { ++tick_; }
    Tick tick() const { return tick_; }
    std::uint64_t frequency() const { return frequency_; }
private:
    std::uint64_t frequency_ = 0, epoch_ = 0;
    Tick tick_ = 0;
};

// Presentation keeps its own absolute phase. A late frame skips superseded
// presentation deadlines without shifting subsequent frames or game ticks.
class PresentationClock {
public:
    void reset(std::uint64_t epoch, std::uint64_t frequency, unsigned rate) {
        epoch_ = deadline_ = epoch; frequency_ = frequency; rate_ = rate;
    }
    bool present(std::uint64_t now, bool ready) {
        if (!frequency_ || !ready || now < deadline_) return false;
        if (!rate_) { deadline_ = now; return true; }
        const auto elapsed = now - epoch_;
        const auto next = (elapsed / frequency_) * rate_ + ((elapsed % frequency_) * rate_) / frequency_ + 1;
        deadline_ = epoch_ + (next / rate_) * frequency_ + ((next % rate_) * frequency_ + rate_ - 1) / rate_;
        return true;
    }
    unsigned rate() const { return rate_; }
    std::uint64_t frequency() const { return frequency_; }
    std::uint64_t deadline() const { return deadline_; }
private:
    std::uint64_t epoch_ = 0, deadline_ = 0, frequency_ = 0;
    unsigned rate_ = 0;
};

// Keep the existing integer phase field while carrying sub-millisecond
// increments outside it. Small ADS/idle speeds must not round to zero per tick.
inline float blend50ms(float reference, float seconds) {
    if (reference <= 0 || seconds <= 0) return 0;
    if (reference >= 1) return 1;
    return static_cast<float>(-std::expm1(std::log1p(-double(reference)) * (double(seconds) / .05)));
}

class FractionalPhase {
public:
    void advance(int& value, double amount) {
        if (value != observed_) remainder_ = 0; // external reset, e.g. respawn
        const double total = amount + remainder_;
        const auto whole = static_cast<std::int64_t>(std::floor(total));
        remainder_ = total - double(whole);
        value = static_cast<int>(static_cast<std::uint32_t>(value) + static_cast<std::uint32_t>(whole));
        observed_ = value;
    }
private:
    int observed_ = 0;
    double remainder_ = 0;
};

// One producer and one consumer. Reject overflow rather than overwrite data
// the consumer may be reading. Memory order publishes the complete record.
template<class T, std::size_t Capacity> class SpscQueue {
    static_assert(Capacity > 1 && (Capacity & (Capacity - 1)) == 0);
public:
    bool push(const T& item) {
        const auto w = write_.load(std::memory_order_relaxed);
        if (w - read_.load(std::memory_order_acquire) >= Capacity) return false;
        data_[w & (Capacity - 1)] = item;
        write_.store(w + 1, std::memory_order_release); return true;
    }
    bool pop(T& item) {
        const auto r = read_.load(std::memory_order_relaxed);
        if (r == write_.load(std::memory_order_acquire)) return false;
        item = data_[r & (Capacity - 1)];
        read_.store(r + 1, std::memory_order_release); return true;
    }
    // The single consumer may inspect the published front without consuming
    // future input. Catch-up ticks must not receive events from after their
    // absolute deadline. The producer cannot reuse this slot until release.
    bool popDue(T& item, std::uint64_t deadline) {
        const auto r = read_.load(std::memory_order_relaxed);
        if (r == write_.load(std::memory_order_acquire)) return false;
        const auto& front = data_[r & (Capacity - 1)];
        if (front.timestamp > deadline) return false;
        item = front;
        read_.store(r + 1, std::memory_order_release); return true;
    }
    std::size_t size() const { return write_.load() - read_.load(); }
private:
    std::array<T, Capacity> data_{};
    alignas(64) std::atomic<std::size_t> write_{0};
    alignas(64) std::atomic<std::size_t> read_{0};
};

// The engine VM can only represent 24-bit array keys. Map 64-bit deadlines
// to independent small keys; their values never encode a truncated clock.
// Fixed-capacity hash table and indexed heap allocate no gameplay memory.
template<std::size_t Capacity> class DeadlineCalendar {
    static_assert((Capacity & (Capacity - 1)) == 0);
    struct Node { Tick deadline = 0; unsigned heap = 0, slot = 0; bool live = false, queued = false; };
    struct Slot { Tick deadline = 0; unsigned key = 0; unsigned char state = 0; };
    static constexpr auto Slots = Capacity * 2;
public:
    DeadlineCalendar() { reset(); }
    void reset() {
        count_ = 0; freeCount_ = Capacity;
        for (unsigned i = 0; i < Capacity; ++i) { nodes_[i] = {}; free_[i] = static_cast<unsigned>(Capacity - i); }
        for (auto& slot : slots_) slot = {};
    }
    unsigned keyFor(Tick deadline) {
        auto slot = hash(deadline); unsigned tombstone = Slots;
        for (unsigned probes = 0; probes < Slots; ++probes, slot = (slot + 1) & (Slots - 1)) {
            auto& entry = slots_[slot];
            if (entry.state == 1 && entry.deadline == deadline) return entry.key;
            if (entry.state == 2 && tombstone == Slots) tombstone = slot;
            if (!entry.state) {
                if (tombstone != Slots) slot = tombstone;
                return insert(deadline, slot);
            }
        }
        return tombstone != Slots ? insert(deadline, tombstone) : 0;
    }
    unsigned takeDue(Tick now) {
        if (!count_ || nodes_[heap_[0] - 1].deadline > now) return 0;
        const auto key = heap_[0]; removeHeap(key); return key;
    }
    void release(unsigned key) {
        if (!key || key > Capacity || !nodes_[key - 1].live) return;
        auto& node = nodes_[key - 1];
        if (node.queued) removeHeap(key);
        unsigned hole = node.slot;
        slots_[hole] = {};
        // Backshift deletion prevents tombstones from accumulating forever.
        for (unsigned scan = (hole + 1) & (Slots - 1); slots_[scan].state;
             scan = (scan + 1) & (Slots - 1)) {
            const auto home = hash(slots_[scan].deadline);
            if (((hole - home) & (Slots - 1)) < ((scan - home) & (Slots - 1))) {
                slots_[hole] = slots_[scan];
                nodes_[slots_[hole].key - 1].slot = hole;
                slots_[scan] = {};
                hole = scan;
            }
        }
        node.live = false; free_[freeCount_++] = key;
    }
    Tick deadlineFor(unsigned key) const { return nodes_[key - 1].deadline; }
    std::size_t size() const { return Capacity - freeCount_; }
private:
    static unsigned hash(Tick value) {
        value ^= value >> 33; value *= 0xff51afd7ed558ccdULL; value ^= value >> 33;
        return static_cast<unsigned>(value) & (Slots - 1);
    }
    unsigned insert(Tick deadline, unsigned slot) {
        if (!freeCount_) return 0;
        const auto key = free_[--freeCount_]; auto& node = nodes_[key - 1];
        node = {deadline, count_, slot, true, true}; slots_[slot] = {deadline, key, 1};
        heap_[count_++] = key; up(node.heap); return key;
    }
    bool before(unsigned a, unsigned b) const { return nodes_[heap_[a] - 1].deadline < nodes_[heap_[b] - 1].deadline; }
    void swap(unsigned a, unsigned b) {
        auto key = heap_[a]; heap_[a] = heap_[b]; heap_[b] = key;
        nodes_[heap_[a] - 1].heap = a; nodes_[heap_[b] - 1].heap = b;
    }
    void up(unsigned index) { while (index && before(index, (index - 1) / 2)) { const auto parent = (index - 1) / 2; swap(index, parent); index = parent; } }
    void down(unsigned index) {
        while (index * 2 + 1 < count_) {
            auto child = index * 2 + 1;
            if (child + 1 < count_ && before(child + 1, child)) ++child;
            if (!before(child, index)) break;
            swap(index, child); index = child;
        }
    }
    void removeHeap(unsigned key) {
        auto& node = nodes_[key - 1]; const auto index = node.heap;
        node.queued = false; --count_;
        if (index == count_) return;
        heap_[index] = heap_[count_]; nodes_[heap_[index] - 1].heap = index;
        if (index && before(index, (index - 1) / 2)) up(index); else down(index);
    }
    std::array<Node, Capacity> nodes_{};
    std::array<Slot, Slots> slots_{};
    std::array<unsigned, Capacity> heap_{}, free_{};
    unsigned count_ = 0, freeCount_ = 0;
};

struct WireHeader { std::uint64_t session = 0, tick = 0, input = 0, ack = 0; };
enum class HeaderDisposition { Valid, StaleSession, Invalid };
inline HeaderDisposition checkClientHeader(const WireHeader& header, std::uint64_t session, Tick serverTick) {
    if (!session || header.input > static_cast<std::uint64_t>((std::numeric_limits<int>::max)())) return HeaderDisposition::Invalid;
    // Queued UDP packets from the previous map are expected during a session
    // transition. They carry no input into the new world and cannot kick a peer.
    if (header.session != session) return HeaderDisposition::StaleSession;
    return header.ack <= serverTick ? HeaderDisposition::Valid : HeaderDisposition::Invalid;
}
inline bool validClientHeader(const WireHeader& header, std::uint64_t session, Tick serverTick) {
    return checkClientHeader(header, session, serverTick) == HeaderDisposition::Valid;
}
inline bool validServerHeader(const WireHeader& header, std::uint64_t lastInput) {
    return header.session && header.input <= header.ack && header.ack <= lastInput;
}
inline constexpr std::size_t WireBytes = 36;
inline void write64(std::uint8_t* out, std::uint64_t value) { for (unsigned i = 0; i < 8; ++i) out[i] = static_cast<std::uint8_t>(value >> (i * 8)); }
inline std::uint64_t read64(const std::uint8_t* in) { std::uint64_t value = 0; for (unsigned i = 0; i < 8; ++i) value |= std::uint64_t(in[i]) << (i * 8); return value; }
inline void encode(std::uint8_t* out, const WireHeader& h) {
    for (unsigned i = 0; i < 4; ++i) out[i] = static_cast<std::uint8_t>(WireMagic >> (i * 8));
    write64(out + 4, h.session); write64(out + 12, h.tick); write64(out + 20, h.input); write64(out + 28, h.ack);
}
inline bool decode(const std::uint8_t* in, std::size_t length, WireHeader& h) {
    if (length < WireBytes) return false;
    for (unsigned i = 0; i < 4; ++i) if (in[i] != static_cast<std::uint8_t>(WireMagic >> (i * 8))) return false;
    h = {read64(in + 4), read64(in + 12), read64(in + 20), read64(in + 28)}; return true;
}

struct Histogram {
    std::array<std::uint64_t, 10001> bins{};
    std::uint64_t count = 0, total = 0, maximum = 0;
    void add(std::uint64_t usec) { ++bins[static_cast<std::size_t>(usec < bins.size() ? usec : bins.size() - 1)]; ++count; total += usec; if (usec > maximum) maximum = usec; }
    unsigned percentile(unsigned percent) const {
        if (!count) return 0; const auto target = (count * percent + 99) / 100; std::uint64_t seen = 0;
        for (unsigned i = 0; i < bins.size(); ++i) { seen += bins[i]; if (seen >= target) return i; }
        return static_cast<unsigned>(bins.size() - 1);
    }
};
std::uint64_t counter();
std::uint64_t frequency();
void waitUntil(std::uint64_t deadline);
// True when the optional Windows event wakes before the absolute deadline;
// false when the deadline wins. The caller rechecks gameplay work first.
bool waitUntilOrEvent(std::uint64_t deadline, void* event);
}
