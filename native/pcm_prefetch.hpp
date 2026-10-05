#pragma once
#include <array>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace khz {
// One decoder owner, bounded look-ahead, nonblocking presentation consumption.
// The caller must destroy/join this owner before closing its decoder or file.
template<unsigned Frames, unsigned Channels = 2, unsigned Depth = 4>
class PcmPrefetch {
public:
    struct Block {
        std::array<std::int16_t, Frames * Channels> samples{};
        std::uint64_t frames = 0;
        bool loopRestart = false;
    };
    using Decode = void (*)(void*, Block&);
private:
    std::array<Block, Depth> blocks_{};
    std::mutex mutex_;
    std::condition_variable changed_;
    unsigned read_ = 0, write_ = 0, count_ = 0;
    bool stop_ = false, finished_ = false;
    std::exception_ptr failure_;
    std::thread worker_;
    void work(Decode decode, void* context) noexcept {
        try {
            Block block;
            for (;;) {
                {
                    std::unique_lock lock(mutex_);
                    changed_.wait(lock, [&] { return stop_ || count_ < Depth; });
                    if (stop_) return;
                }
                block.frames = 0; block.loopRestart = false;
                decode(context, block);
                std::lock_guard lock(mutex_);
                if (stop_) return;
                if (!block.frames) { finished_ = true; return; }
                if (block.frames > Frames) throw std::runtime_error("PCM decoder exceeded its chunk bound");
                blocks_[write_] = block; write_ = (write_ + 1) % Depth; ++count_;
            }
        } catch (...) {
            std::lock_guard lock(mutex_);
            failure_ = std::current_exception(); finished_ = true;
        }
    }
public:
    PcmPrefetch(Decode decode, void* context) : worker_([=, this] { work(decode, context); }) {}
    PcmPrefetch(const PcmPrefetch&) = delete;
    PcmPrefetch& operator=(const PcmPrefetch&) = delete;
    ~PcmPrefetch() {
        { std::lock_guard lock(mutex_); stop_ = true; }
        changed_.notify_one(); worker_.join();
    }
    bool pop(Block& out) {
        std::lock_guard lock(mutex_);
        if (failure_) std::rethrow_exception(failure_);
        if (!count_) return false;
        out = blocks_[read_]; read_ = (read_ + 1) % Depth; --count_;
        changed_.notify_one(); return true;
    }
    bool pending() {
        std::lock_guard lock(mutex_);
        if (failure_) std::rethrow_exception(failure_);
        return count_ || !finished_;
    }
};
} // namespace khz
