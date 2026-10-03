#include <plugin_packet_mailbox.hpp>
#include <array>
#include <atomic>
#include <cassert>
#include <thread>
#include <cstdio>
int main() {
    using Queue = khz::plugins::PacketMailbox<unsigned, 16, 32>;
    Queue queue; Queue::Packet output; Queue::Token token{3, 9, 7, 11, 48};
    std::array<unsigned char, 32> input{}; input[0] = 17; input[31] = 255;
    assert(!queue.Push(token, 4, nullptr, 0));
    assert(!queue.Push(token, 4, input.data(), -1));
    assert(!queue.Push(token, 4, input.data(), 33));
    for (unsigned i = 0; i != 16; ++i) assert(queue.Push(token, i, input.data(), int(input.size())));
    assert(!queue.Push(token, 99, input.data(), 1)); input.fill(0);
    std::array<bool, 16> seen{};
    for (unsigned i = 0; i != 16; ++i) {
        assert(queue.Pop(output)); assert(output.endpoint == i && !seen[output.endpoint]); seen[output.endpoint] = true;
        assert(output.length == 32 && output.data[0] == 17 && output.data[31] == 255);
        assert(output.token.owner == 3 && output.token.generation == 9 && output.token.epoch == 7 && output.token.session == 11 && output.token.serverId == 48);
    }
    assert(!queue.Pop(output));
    constexpr unsigned Producers = 4, Each = 1500; std::atomic<unsigned> running{Producers};
    std::array<std::thread, Producers> workers;
    for (unsigned producer = 0; producer != Producers; ++producer) workers[producer] = std::thread([&, producer] {
        for (unsigned i = 0; i != Each; ++i) {
            const unsigned value = producer * Each + i;
            Queue::Token own{producer, 100 + producer, 77, 123, 32};
            while (!queue.Push(own, value, &value, sizeof(value))) std::this_thread::yield();
        }
        --running;
    });
    std::array<bool, Producers * Each> all{}; std::array<unsigned, Producers> expectedPerProducer{}; unsigned total = 0;
    while (running || total != all.size()) {
        if (!queue.Pop(output)) { std::this_thread::yield(); continue; }
        unsigned bytes = 0; std::memcpy(&bytes, output.data.data(), sizeof(bytes));
        assert(output.length == sizeof(bytes) && bytes == output.endpoint && bytes < all.size() && !all[bytes]);
        assert(output.token.owner == bytes / Each && output.token.generation == 100 + bytes / Each);
        assert(bytes % Each == expectedPerProducer[bytes / Each]++);
        all[bytes] = true; ++total;
    }
    for (auto& worker : workers) worker.join();
    assert(total == Producers * Each && !queue.Pop(output));
    std::puts("plugin packet mailbox: bounded copied payload and 6000 concurrent deliveries pass");
}
