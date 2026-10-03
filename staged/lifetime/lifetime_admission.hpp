#pragma once
#include <atomic>
#include <cstdint>
#include <cstdlib>

// STAGED policy only. Native source-owned wrappers must supply the actual
// thread ID and derive bound-tree eligibility from the pinned native table.
// No generic policy can enroll missing DB/hunk/XAnim lifetime entrypoints.
namespace khz::lifetime {
struct ThreadState {
    unsigned depth = 0;
    std::uint64_t epoch = 1;
};

class Admission {
public:
    static constexpr unsigned Closed = 0x80000000u;
    static constexpr unsigned ActiveMask = ~Closed;
    // This is a representable counter limit, not an artificial recursion or
    // contention budget. Ordinary unpinned async admission is never refused
    // merely because eight compare-exchanges lost to concurrent callers.
    static constexpr unsigned MaximumDepth = ActiveMask;

    class Lease {
    public:
        template<class Eligibility>
        Lease(Admission& gate, ThreadState& thread, unsigned caller, Eligibility&& boundTree)
            : gate_(&gate), thread_(&thread), epoch_(thread.epoch),
              admitted_(gate.enter(thread, caller, boundTree, boundProof_)) {
            if (admitted_) ++thread.depth;
        }
        ~Lease() {
            if (!admitted_ || epoch_ != thread_->epoch) return;
            if (!thread_->depth) std::abort();
            --thread_->depth;
            const auto prior = gate_->state_.fetch_sub(1, std::memory_order_seq_cst);
            if (!(prior & ActiveMask)) std::abort();
        }
        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;
        bool allowed() const { return admitted_; }
        bool usedBoundProof() const { return admitted_ && boundProof_; }
    private:
        Admission* gate_;
        ThreadState* thread_;
        std::uint64_t epoch_;
        bool boundProof_ = false;
        bool admitted_;
    };

    bool bindOwner(unsigned caller) {
        if (!caller || state_.load(std::memory_order_seq_cst)) return false;
        unsigned expected = 0;
        return owner_.compare_exchange_strong(expected, caller, std::memory_order_seq_cst) || expected == caller;
    }
    bool close(unsigned caller) {
        if (!caller || owner_.load(std::memory_order_seq_cst) != caller) return false;
        // Publish closure even when an earlier complete-function lease is
        // still active. The observer must refuse all reads until closedIdle;
        // explicit Empty cleanup retains/releases this same closed gate.
        return (state_.fetch_or(Closed, std::memory_order_seq_cst) & ActiveMask) == 0;
    }
    bool closed(unsigned caller) const {
        return caller && owner_.load(std::memory_order_seq_cst) == caller &&
            (state_.load(std::memory_order_seq_cst) & Closed) != 0;
    }
    bool closedIdle(unsigned caller) const {
        return caller && owner_.load(std::memory_order_seq_cst) == caller && state_.load(std::memory_order_seq_cst) == Closed;
    }
    bool release(unsigned caller) {
        if (!caller || owner_.load(std::memory_order_seq_cst) != caller) return false;
        unsigned expected = Closed;
        return state_.compare_exchange_strong(expected, 0, std::memory_order_seq_cst);
    }
    // Called only at a native nonreturning error jump, never a returning notice.
    // Preserve other threads' complete leases and the exclusive closed bit.
    bool abandon(ThreadState& thread) {
        if (!thread.depth) return true;
        if (thread.epoch == UINT64_MAX) return false;
        auto state = state_.load(std::memory_order_seq_cst);
        for (;;) {
            if ((state & ActiveMask) < thread.depth) return false;
            if (state_.compare_exchange_strong(state, state - thread.depth, std::memory_order_seq_cst)) {
                abandoned_.fetch_add(thread.depth, std::memory_order_seq_cst);
                thread.depth = 0;
                ++thread.epoch;
                return true;
            }
        }
    }
    unsigned active() const { return state_.load(std::memory_order_seq_cst) & ActiveMask; }
    unsigned refusals() const { return refused_.load(std::memory_order_seq_cst); }
    std::uint64_t abandoned() const { return abandoned_.load(std::memory_order_seq_cst); }
private:
    template<class Eligibility>
    bool enter(const ThreadState& thread, unsigned caller, Eligibility& boundTree, bool& boundProof) {
        if (!caller) { refused_.fetch_add(1); return false; }
        auto state = state_.load(std::memory_order_seq_cst);
        for (;;) {
            // Open admission preserves ordinary asynchronous DB behavior.
            // Closed admission permits only actual-main mutation of a bound
            // restorable tree. Asset/hunk lifetime operations pass false.
            if ((state & ActiveMask) >= MaximumDepth) break;
            if (state & Closed) {
                // Lazy proof cannot touch a bound tree/provider table from a
                // foreign thread, nor while any pre-closure foreign lease can
                // still replace/free its assets. Only this caller's nested
                // complete scopes may remain active during an owner proof.
                if (owner_.load(std::memory_order_seq_cst) != caller ||
                    (state & ActiveMask) != thread.depth || !boundTree()) break;
                boundProof = true;
            } else boundProof = false;
            if (state_.compare_exchange_strong(state, state + 1, std::memory_order_seq_cst)) return true;
        }
        refused_.fetch_add(1, std::memory_order_seq_cst);
        return false;
    }
    std::atomic<unsigned> state_{0}, owner_{0}, refused_{0};
    std::atomic<std::uint64_t> abandoned_{0};
};
}
