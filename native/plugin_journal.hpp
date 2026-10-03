#pragma once
#include <array>
#include <cstdint>
#include <cstring>

namespace khz::plugins {
// One-owner log journal. Bind/Invalidate are lifecycle barriers; a native file
// handle recycled by the filesystem never inherits records from its old owner.
template<unsigned Capacity, unsigned Bytes> class FileJournal {
public:
    struct Record { std::uint64_t generation = 0; int handle = 0; char text[Bytes]{}; };
    unsigned Bind(int handle) {
        const unsigned discarded = Invalidate(); handle_ = handle; return discarded;
    }
    unsigned Invalidate() {
        const unsigned discarded = count_; head_ = count_ = 0; handle_ = 0; ++generation_; return discarded;
    }
    bool Push(const char* text) {
        if (!handle_ || !text || count_ == Capacity) return false;
        auto& entry = entries_[(head_ + count_++) % Capacity];
        entry.handle = handle_; entry.generation = generation_;
        unsigned i = 0; for (; i + 1 < Bytes && text[i]; ++i) entry.text[i] = text[i]; entry.text[i] = 0;
        return true;
    }
    bool Pop(Record& entry) {
        if (!count_) return false;
        entry = entries_[head_]; head_ = (head_ + 1) % Capacity; --count_; return true;
    }
    bool Active(const Record& entry) const { return handle_ && entry.handle == handle_ && entry.generation == generation_; }
    int Handle() const { return handle_; }
    unsigned Count() const { return count_; }
private:
    std::array<Record, Capacity> entries_{};
    std::uint64_t generation_ = 0;
    int handle_ = 0;
    unsigned head_ = 0, count_ = 0;
};
}
