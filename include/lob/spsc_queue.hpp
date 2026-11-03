// Bounded single-producer / single-consumer lock-free ring buffer.
//
// Exactly ONE thread may call try_push and exactly ONE (other) thread may call
// try_pop. Under that contract no locks or read-modify-write atomics are
// needed: each index has a single writer.
//
// Indices (`head_` = next slot to read, `tail_` = next slot to write) only ever
// increase; the slot is `index & mask_`. size_t is 64-bit so wrap-around of the
// counters is not a practical concern, and "full" is simply tail - head ==
// capacity.
//
// Memory ordering (the part worth being able to explain):
//   * The producer writes the slot, THEN does tail_.store(release).
//   * The consumer does tail_.load(acquire), THEN reads the slot.
//     The release/acquire pair makes the slot write visible before the new
//     tail value is. Symmetrically for head_: the consumer finishes reading a
//     slot before head_.store(release), and the producer's head_.load(acquire)
//     guarantees it will not overwrite a slot that is still being read.
//   * Each side reads its OWN index with relaxed ordering (nobody else writes it).
//
// Performance detail: each side caches the other side's last-seen index
// (cached_head_ / cached_tail_) and only re-reads the shared atomic when the
// cache says "full" / "empty". That avoids bouncing the cache line on every
// operation. head_ and tail_ live on separate cache lines to avoid false sharing.
#pragma once

#include <atomic>
#include <bit>
#include <cstddef>
#include <memory>
#include <type_traits>

namespace lob {

template <typename T>
class SpscQueue {
  static_assert(std::is_trivially_copyable_v<T>,
                "slots are copied with plain assignment; keep T simple");

 public:
  // Capacity is rounded up to a power of two (so index & mask replaces %).
  explicit SpscQueue(std::size_t min_capacity)
      : capacity_(std::bit_ceil(min_capacity < 2 ? std::size_t{2} : min_capacity)),
        mask_(capacity_ - 1),
        buffer_(std::make_unique<T[]>(capacity_)) {}

  SpscQueue(const SpscQueue&) = delete;
  SpscQueue& operator=(const SpscQueue&) = delete;

  std::size_t capacity() const noexcept { return capacity_; }

  // Producer thread only. Returns false if the queue is full.
  bool try_push(const T& value) {
    const std::size_t tail = tail_.load(std::memory_order_relaxed);
    if (tail - cached_head_ == capacity_) {
      cached_head_ = head_.load(std::memory_order_acquire);
      if (tail - cached_head_ == capacity_) return false;  // really full
    }
    buffer_[tail & mask_] = value;
    tail_.store(tail + 1, std::memory_order_release);
    return true;
  }

  // Consumer thread only. Returns false if the queue is empty.
  bool try_pop(T& out) {
    const std::size_t head = head_.load(std::memory_order_relaxed);
    if (head == cached_tail_) {
      cached_tail_ = tail_.load(std::memory_order_acquire);
      if (head == cached_tail_) return false;  // really empty
    }
    out = buffer_[head & mask_];
    head_.store(head + 1, std::memory_order_release);
    return true;
  }

 private:
  static constexpr std::size_t kCacheLine = 64;

  const std::size_t capacity_;
  const std::size_t mask_;
  std::unique_ptr<T[]> buffer_;

  // Consumer-owned line: head_ and the consumer's cache of tail_.
  alignas(kCacheLine) std::atomic<std::size_t> head_{0};
  std::size_t cached_tail_ = 0;

  // Producer-owned line: tail_ and the producer's cache of head_.
  alignas(kCacheLine) std::atomic<std::size_t> tail_{0};
  std::size_t cached_head_ = 0;
};

}  // namespace lob
