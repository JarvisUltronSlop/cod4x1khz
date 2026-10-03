#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>

namespace khz::plugins {
// Multiple pinned producers, one native server owner. A producer owns a slot
// until it publishes Ready; cancellation never rewrites a Writing slot. The
// consumer rejects obsolete owner/session/lifecycle tokens before dispatch.
template<class Endpoint, unsigned Capacity = 16, unsigned Bytes = 65507>
class PacketMailbox {
public:
    struct Token {
        unsigned owner = 0;
        std::uint64_t generation = 0, epoch = 0, session = 0;
        int serverId = 0;
    };
    struct Packet {
        Token token{}; Endpoint endpoint{}; unsigned length = 0;
        std::array<unsigned char, Bytes> data{};
    };
    bool Push(const Token& token, const Endpoint& endpoint, const void* bytes, int length) {
        if (!bytes || length < 0 || unsigned(length) > Bytes) return false;
        for (auto& slot : slots_) {
            unsigned expected = Free;
            if (!slot.state.compare_exchange_strong(expected, Writing, std::memory_order_acquire)) continue;
            slot.packet.token = token; slot.packet.endpoint = endpoint; slot.packet.length = unsigned(length);
            if (length) std::memcpy(slot.packet.data.data(), bytes, unsigned(length));
            slot.sequence = sequence_.fetch_add(1, std::memory_order_relaxed);
            slot.state.store(Ready, std::memory_order_release); return true;
        }
        return false;
    }
    bool Pop(Packet& output) {
        Slot* selected = nullptr;
        // A publisher can become Ready after the consumer has inspected its
        // slot. Never skip that reserved sequence in favor of a later one.
        // Return immediately if it is still Writing; no owner-side spin.
        for (auto& slot : slots_) if (slot.state.load(std::memory_order_acquire) == Ready && slot.sequence == next_) {
            selected = &slot; break;
        }
        if (!selected) return false;
        selected->state.store(Reading, std::memory_order_relaxed);
        output.token = selected->packet.token; output.endpoint = selected->packet.endpoint; output.length = selected->packet.length;
        if (output.length) std::memcpy(output.data.data(), selected->packet.data.data(), output.length);
        selected->state.store(Free, std::memory_order_release); ++next_; return true;
    }
private:
    enum : unsigned { Free, Writing, Ready, Reading };
    struct Slot { std::atomic<unsigned> state{Free}; std::uint64_t sequence = 0; Packet packet{}; };
    std::array<Slot, Capacity> slots_{};
    std::atomic<std::uint64_t> sequence_{0};
    std::uint64_t next_ = 0; // single consumer only
};
} // namespace khz::plugins
