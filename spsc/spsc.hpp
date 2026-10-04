// spsc.hpp - bounded single-producer / single-consumer lock-free ring buffer.
//
// Rules:
//   * Exactly ONE thread calls try_push, exactly ONE thread calls try_pop.
//   * N must be a power of two (index wrap is a mask, not a %).
//
// Memory ordering:
//   producer: write slot  -> tail_.store(release)   "publish"
//   consumer: tail_.load(acquire) -> read slot      "see the published slot"
//   (and symmetrically head_ for "slot is free again")
#pragma once
#include <atomic>
#include <cstddef>
#include <new>
#include <type_traits>

template <typename T, std::size_t N>
class SPSCQueue {
    static_assert(N >= 2 && (N & (N - 1)) == 0, "N must be a power of two");
    static_assert(std::is_trivially_copyable_v<T>, "T should be trivially copyable");
    static constexpr std::size_t kMask = N - 1;
    static constexpr std::size_t kCL   = 64;   // cache-line size

public:
    // Producer side ------------------------------------------------------
    bool try_push(const T& v) noexcept {
        const std::size_t t = tail_.load(std::memory_order_relaxed);   // only we write tail_
        if (t - cached_head_ == N) {                                   // looks full?
            cached_head_ = head_.load(std::memory_order_acquire);      // refresh from consumer
            if (t - cached_head_ == N) return false;                   // really full
        }
        buf_[t & kMask] = v;
        tail_.store(t + 1, std::memory_order_release);                 // publish slot
        return true;
    }

    // Consumer side ------------------------------------------------------
    bool try_pop(T& out) noexcept {
        const std::size_t h = head_.load(std::memory_order_relaxed);   // only we write head_
        if (h == cached_tail_) {                                       // looks empty?
            cached_tail_ = tail_.load(std::memory_order_acquire);      // refresh from producer
            if (h == cached_tail_) return false;                       // really empty
        }
        out = buf_[h & kMask];
        head_.store(h + 1, std::memory_order_release);                 // free slot
        return true;
    }

    // Approximate; only for stats, never for correctness.
    std::size_t size_approx() const noexcept {
        return tail_.load(std::memory_order_relaxed) - head_.load(std::memory_order_relaxed);
    }

private:
    // Consumer-owned line: head_ + consumer's cached copy of tail_.
    alignas(kCL) std::atomic<std::size_t> head_{0};
    std::size_t cached_tail_{0};

    // Producer-owned line: tail_ + producer's cached copy of head_.
    alignas(kCL) std::atomic<std::size_t> tail_{0};
    std::size_t cached_head_{0};

    alignas(kCL) T buf_[N];
};

