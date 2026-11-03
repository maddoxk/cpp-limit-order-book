// End-to-end throughput on a synthetic seeded feed (seed 42, 500k messages):
//   * apply only        - messages already decoded, measures the book
//   * decode + apply    - one thread
//   * decode | SPSC | apply - two threads
// The feed is generated once, outside the timed region.
#include <benchmark/benchmark.h>

#include <vector>

#include "lob/generator.hpp"
#include "lob/replay.hpp"

namespace {

using namespace lob;

constexpr std::size_t kMessages = 500'000;

const std::vector<Message>& messages() {
  static const std::vector<Message> m = [] {
    GeneratorConfig cfg;
    cfg.seed = 42;
    return FeedGenerator(cfg).generate(kMessages);
  }();
  return m;
}

const std::vector<std::uint8_t>& feed_bytes() {
  static const std::vector<std::uint8_t> b = encode_feed(messages());
  return b;
}

void BM_ApplyPreDecoded(benchmark::State& state) {
  const auto& msgs = messages();
  for (auto _ : state) {
    OrderBook book(1 << 16);
    ReplayStats stats;
    for (const Message& m : msgs) apply(book, m, stats);
    benchmark::DoNotOptimize(stats.trade_checksum);
  }
  state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(kMessages));
}
BENCHMARK(BM_ApplyPreDecoded)->Unit(benchmark::kMillisecond);

void BM_ReplayDecodeAndApply1Thread(benchmark::State& state) {
  const auto& bytes = feed_bytes();
  for (auto _ : state) {
    benchmark::DoNotOptimize(replay_bytes(bytes).stats.trade_checksum);
  }
  state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(kMessages));
}
BENCHMARK(BM_ReplayDecodeAndApply1Thread)->Unit(benchmark::kMillisecond);

void BM_ReplayTwoThreadsSpsc(benchmark::State& state) {
  const auto& bytes = feed_bytes();
  for (auto _ : state) {
    benchmark::DoNotOptimize(
        replay_bytes_threaded(bytes, 1 << 14).stats.trade_checksum);
  }
  state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(kMessages));
}
// Wall-clock time: the work is spread over two threads.
BENCHMARK(BM_ReplayTwoThreadsSpsc)->UseRealTime()->Unit(benchmark::kMillisecond);

}  // namespace
