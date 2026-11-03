#include <gtest/gtest.h>

#include <vector>

#include "lob/generator.hpp"
#include "lob/replay.hpp"

namespace {

using namespace lob;

std::vector<Message> gen(std::uint64_t seed, std::size_t n) {
  GeneratorConfig cfg;
  cfg.seed = seed;
  return FeedGenerator(cfg).generate(n);
}

TEST(Generator, SameSeedSameOutputDifferentSeedDiffers) {
  EXPECT_EQ(gen(7, 5000), gen(7, 5000));
  EXPECT_NE(gen(7, 5000), gen(8, 5000));
}

TEST(Generator, FeedHasAllMessageKindsAndNoRejects) {
  const auto bytes = encode_feed(gen(3, 50'000));
  const ReplayResult r = replay_bytes(bytes);
  ASSERT_TRUE(r.header_ok);
  ASSERT_EQ(r.decode_status, DecodeStatus::Ok);
  const ReplayStats& s = r.stats;
  EXPECT_EQ(s.messages, 50'000u);
  EXPECT_GT(s.limit_orders, 0u);
  EXPECT_GT(s.market_orders, 0u);
  EXPECT_GT(s.cancels, 0u);
  EXPECT_GT(s.modifies, 0u);
  EXPECT_GT(s.executes, 0u);
  EXPECT_GT(s.trades, 0u);
  // Every cancel/modify/execute targets a genuinely resting order.
  EXPECT_EQ(s.rejects, 0u);
  EXPECT_GT(s.resting_orders, 0u);
  ASSERT_TRUE(s.best_bid && s.best_ask);
  EXPECT_LT(*s.best_bid, *s.best_ask);
}

TEST(Replay, IsDeterministic) {
  const auto bytes = encode_feed(gen(11, 30'000));
  EXPECT_EQ(replay_bytes(bytes).stats, replay_bytes(bytes).stats);
}

TEST(Replay, ThreadedPipelineMatchesSingleThread) {
  const auto bytes = encode_feed(gen(5, 60'000));
  const ReplayStats single = replay_bytes(bytes).stats;
  for (std::size_t cap : {2u, 64u, 1u << 14}) {  // tiny queues stress the full path
    const ReplayResult t = replay_bytes_threaded(bytes, cap);
    EXPECT_EQ(t.decode_status, DecodeStatus::Ok);
    EXPECT_EQ(t.stats, single) << "queue capacity " << cap;
  }
}

TEST(Replay, BadHeaderIsReported) {
  std::vector<std::uint8_t> bytes = encode_feed(gen(1, 10));
  bytes[1] = 'Q';
  EXPECT_FALSE(replay_bytes(bytes).header_ok);
  EXPECT_FALSE(replay_bytes_threaded(bytes, 8).header_ok);
}

TEST(Replay, TruncatedFileStopsWithNeedMoreAfterProcessingWhatItCan) {
  std::vector<std::uint8_t> bytes = encode_feed(gen(1, 100));
  bytes.pop_back();
  const ReplayResult a = replay_bytes(bytes);
  const ReplayResult b = replay_bytes_threaded(bytes, 8);
  EXPECT_EQ(a.decode_status, DecodeStatus::NeedMore);
  EXPECT_EQ(a.stats.messages, 99u);
  EXPECT_EQ(b.decode_status, DecodeStatus::NeedMore);
  EXPECT_EQ(b.stats, a.stats);
}

TEST(Replay, CorruptTypeByteStopsWithBadType) {
  std::vector<std::uint8_t> bytes = encode_feed(gen(1, 100));
  bytes[kFileHeaderSize] = 'Z';
  EXPECT_EQ(replay_bytes(bytes).decode_status, DecodeStatus::BadType);
  EXPECT_EQ(replay_bytes(bytes).stats.messages, 0u);
}

}  // namespace
