// Run under ThreadSanitizer in CI (the "tsan" job) to check the memory
// ordering of the two-thread test.
#include <gtest/gtest.h>

#include <cstdint>
#include <thread>

#include "lob/spsc_queue.hpp"

namespace {

using lob::SpscQueue;

TEST(Spsc, CapacityRoundsUpToPowerOfTwo) {
  EXPECT_EQ(SpscQueue<int>(5).capacity(), 8u);
  EXPECT_EQ(SpscQueue<int>(8).capacity(), 8u);
  EXPECT_EQ(SpscQueue<int>(0).capacity(), 2u);
}

TEST(Spsc, EmptyPopFailsAndFullPushFails) {
  SpscQueue<int> q(4);
  int v = -1;
  EXPECT_FALSE(q.try_pop(v));
  for (int i = 0; i < 4; ++i) EXPECT_TRUE(q.try_push(i));
  EXPECT_FALSE(q.try_push(99));
  EXPECT_TRUE(q.try_pop(v));
  EXPECT_EQ(v, 0);
  EXPECT_TRUE(q.try_push(4));  // a slot freed up
}

TEST(Spsc, FifoOrderAcrossManyWraparounds) {
  SpscQueue<std::uint64_t> q(4);
  std::uint64_t next_in = 0, next_out = 0;
  for (int round = 0; round < 1000; ++round) {
    for (int k = 0; k < 3; ++k) ASSERT_TRUE(q.try_push(next_in++));
    for (int k = 0; k < 3; ++k) {
      std::uint64_t v;
      ASSERT_TRUE(q.try_pop(v));
      ASSERT_EQ(v, next_out++);
    }
  }
}

TEST(Spsc, TwoThreadsTransferEverythingInOrder) {
  constexpr std::uint64_t kCount = 300'000;
  SpscQueue<std::uint64_t> q(64);  // small, so the full/empty paths are hot

  std::thread producer([&] {
    for (std::uint64_t i = 0; i < kCount; ++i) {
      while (!q.try_push(i)) std::this_thread::yield();
    }
  });

  std::uint64_t expected = 0;
  std::uint64_t sum = 0;
  bool in_order = true;
  while (expected < kCount) {
    std::uint64_t v;
    if (q.try_pop(v)) {
      if (v != expected) in_order = false;
      sum += v;
      ++expected;
    } else {
      std::this_thread::yield();
    }
  }
  producer.join();
  EXPECT_TRUE(in_order);
  EXPECT_EQ(sum, kCount * (kCount - 1) / 2);
}

}  // namespace
