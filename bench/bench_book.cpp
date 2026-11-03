// Micro-benchmarks of the book itself. Each benchmark iteration processes
// kOps operations (reported as items/s). Setup that is not being measured is
// done between state.PauseTiming() / ResumeTiming().
#include <benchmark/benchmark.h>

#include <algorithm>
#include <numeric>
#include <random>
#include <vector>

#include "lob/order_book.hpp"

namespace {

using namespace lob;

constexpr std::size_t kOps = 20'000;
constexpr Price kMid = 10'000;
constexpr int kLevels = 100;  // distinct price levels per side

// Add kOps passive limit orders that never cross: bids below the mid, asks
// above, spread over kLevels levels. Includes the cost of building the book.
void BM_AddPassive(benchmark::State& state) {
  for (auto _ : state) {
    OrderBook book(kOps);
    for (std::size_t i = 0; i < kOps; ++i) {
      const Price off = 1 + static_cast<Price>(i % kLevels);
      const bool buy = i & 1;
      benchmark::DoNotOptimize(book.add_limit(
          i + 1, buy ? Side::Buy : Side::Sell, buy ? kMid - off : kMid + off, 10));
    }
    benchmark::ClobberMemory();
  }
  state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(kOps));
}
BENCHMARK(BM_AddPassive);

// Cancel kOps resting orders in random order (book build is not timed).
void BM_CancelRandomOrder(benchmark::State& state) {
  std::vector<OrderId> ids(kOps);
  std::iota(ids.begin(), ids.end(), OrderId{1});
  std::mt19937 rng(123);
  std::shuffle(ids.begin(), ids.end(), rng);

  for (auto _ : state) {
    state.PauseTiming();
    OrderBook book(kOps);
    for (std::size_t i = 0; i < kOps; ++i) {
      const Price off = 1 + static_cast<Price>(i % kLevels);
      const bool buy = i & 1;
      book.add_limit(i + 1, buy ? Side::Buy : Side::Sell, buy ? kMid - off : kMid + off, 10);
    }
    state.ResumeTiming();
    for (OrderId id : ids) benchmark::DoNotOptimize(book.cancel(id));
  }
  state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(kOps));
}
BENCHMARK(BM_CancelRandomOrder);

// Matching: a deep ask side, then kOps single-share market buys, each of which
// produces one trade against the best level (build is not timed).
void BM_MatchMarketOrders(benchmark::State& state) {
  for (auto _ : state) {
    state.PauseTiming();
    OrderBook book(kOps);
    for (std::size_t i = 0; i < kOps; ++i) {
      book.add_limit(i + 1, Side::Sell, kMid + 1 + static_cast<Price>(i % kLevels), 1);
    }
    state.ResumeTiming();
    for (std::size_t i = 0; i < kOps; ++i) {
      benchmark::DoNotOptimize(book.add_market(kOps + i + 1, Side::Buy, 1));
    }
  }
  state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(kOps));
}
BENCHMARK(BM_MatchMarketOrders);

// One large aggressive order sweeping many levels and many resting orders.
void BM_SweepWholeBook(benchmark::State& state) {
  for (auto _ : state) {
    state.PauseTiming();
    OrderBook book(kOps);
    for (std::size_t i = 0; i < kOps; ++i) {
      book.add_limit(i + 1, Side::Sell, kMid + 1 + static_cast<Price>(i % kLevels), 5);
    }
    state.ResumeTiming();
    benchmark::DoNotOptimize(book.add_market(kOps + 1, Side::Buy, 5 * kOps));
  }
  // Items are resting orders consumed (= trades produced).
  state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(kOps));
}
BENCHMARK(BM_SweepWholeBook);

}  // namespace
