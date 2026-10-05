#include <pcm_prefetch.hpp>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <memory>
#include <stdexcept>
using Queue = khz::PcmPrefetch<16, 2, 4>;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "%d: %s\n", __LINE__, #x); std::exit(1); } } while (0)
struct Decoder { std::atomic<unsigned> read = 0; std::promise<void> entered; std::shared_future<void> release; bool fail = false; };
void decode(void* context, Queue::Block& block) {
    auto& d = *static_cast<Decoder*>(context);
    const auto n = d.read++;
    if (!n && d.release.valid()) { d.entered.set_value(); d.release.wait(); }
    if (d.fail) throw std::runtime_error("decoder failure");
    if (n == 128) return;
    block.frames = n % 16 + 1; block.loopRestart = n % 7 == 0;
    for (unsigned i = 0; i != block.frames * 2; ++i) block.samples[i] = std::int16_t(n * 32 + i);
}
int main() {
    Decoder ordered;
    {
        Queue queue(decode, &ordered);
        unsigned received = 0; Queue::Block block;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (queue.pending()) {
            CHECK(std::chrono::steady_clock::now() < deadline);
            if (!queue.pop(block)) { std::this_thread::yield(); continue; }
            CHECK(block.frames == received % 16 + 1 && block.loopRestart == (received % 7 == 0));
            for (unsigned i = 0; i != block.frames * 2; ++i) CHECK(block.samples[i] == received * 32 + i);
            ++received;
        }
        CHECK(received == 128);
    }
    Decoder stalled; std::promise<void> release;
    stalled.release = release.get_future().share();
    auto queue = std::make_unique<Queue>(decode, &stalled);
    CHECK(stalled.entered.get_future().wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    Queue::Block block; CHECK(!queue->pop(block) && queue->pending());
    std::promise<void> stopping;
    auto joined = std::async(std::launch::async, [&] { stopping.set_value(); queue.reset(); });
    stopping.get_future().wait();
    CHECK(joined.wait_for(std::chrono::milliseconds(10)) == std::future_status::timeout);
    release.set_value(); CHECK(joined.wait_for(std::chrono::seconds(5)) == std::future_status::ready); joined.get();
    Decoder bounded;
    { Queue full(decode, &bounded);
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
      while (bounded.read < 4) { CHECK(std::chrono::steady_clock::now() < deadline); std::this_thread::yield(); }
      CHECK(bounded.read == 4);
    }
    Decoder failure; failure.fail = true;
    { Queue failed(decode, &failure); bool caught = false;
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
      while (!caught) { CHECK(std::chrono::steady_clock::now() < deadline); try { failed.pending(); } catch (const std::runtime_error&) { caught = true; } }
    }
    std::puts("PCM prefetch: ordered stereo/loop chunks, EOF, bounded look-ahead, nonblocking consume, joined shutdown and failure pass");
}
