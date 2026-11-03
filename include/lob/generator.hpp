// Seeded synthetic feed generator.
//
// This is NOT market data. It produces a plausible-looking stream: a mid price
// doing a slow random walk, passive orders placed a geometrically-distributed
// number of ticks away from it, occasional aggressive (crossing) orders and
// market orders, and cancels / modifies / executions that always refer to
// orders that are really resting (the generator runs a private shadow
// OrderBook to know which ids are live).
//
// Same seed + same config + same build => identical output. The random numbers
// come from std::mt19937_64 (fully specified by the standard) and we do our own
// mapping to ranges, because std::*_distribution is not portable across
// standard libraries. The only floating point is log() when drawing sizes and
// tick offsets; it is rounded down to integers.
#pragma once

#include <cstddef>
#include <cstdint>
#include <random>
#include <vector>

#include "lob/messages.hpp"
#include "lob/order_book.hpp"
#include "lob/replay.hpp"

namespace lob {

struct GeneratorConfig {
  std::uint64_t seed = 1;
  Price start_mid = 10'000;  // ticks
  // Relative frequencies of message kinds (normalised internally).
  double w_limit = 0.50;
  double w_market = 0.06;
  double w_cancel = 0.27;
  double w_modify = 0.10;
  double w_execute = 0.07;
  // Share of limit orders priced to cross the spread.
  double p_aggressive = 0.05;
  // Mean distance (ticks) of passive orders from the mid, and mean order size.
  double mean_offset_ticks = 4.0;
  double mean_qty = 30.0;
  // The first `warmup` messages are all passive adds so the book has depth.
  std::size_t warmup = 500;
};

class FeedGenerator {
 public:
  explicit FeedGenerator(const GeneratorConfig& cfg = {});

  Message next();
  std::vector<Message> generate(std::size_t count);

 private:
  std::uint64_t rand64() { return rng_(); }
  std::uint32_t below(std::uint32_t n) { return static_cast<std::uint32_t>(rand64() % n); }
  double unit();                       // uniform in [0, 1)
  std::uint32_t expo(double mean);     // floor of an exponential draw
  Side random_side() { return below(2) ? Side::Buy : Side::Sell; }

  Message make_limit();
  Message make_market_order();
  // Picks a random order that is still resting; returns false if none.
  bool pick_live(OrderId& id, bool remove_from_live);

  GeneratorConfig cfg_;
  std::mt19937_64 rng_;
  OrderBook shadow_;
  ReplayStats shadow_stats_;  // unused output of apply(); required by its signature
  Price mid_;
  OrderId next_id_ = 1;
  std::size_t produced_ = 0;
  std::vector<OrderId> live_;  // ids that were resting when added (pruned lazily)
};

}  // namespace lob
