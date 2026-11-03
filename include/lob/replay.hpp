// Driving an OrderBook from messages, and summarising what happened.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "lob/messages.hpp"
#include "lob/order_book.hpp"

namespace lob {

struct ReplayStats {
  std::uint64_t messages = 0;
  std::uint64_t limit_orders = 0;
  std::uint64_t market_orders = 0;
  std::uint64_t cancels = 0;
  std::uint64_t modifies = 0;
  std::uint64_t executes = 0;
  std::uint64_t rejects = 0;      // messages the book refused (should be 0 for generated feeds)
  std::uint64_t trades = 0;       // individual fills
  std::uint64_t traded_qty = 0;   // sum of fill quantities
  // FNV-1a hash over every trade (maker, taker, price, qty) in order. Two runs
  // that produce the same trades in the same order have the same checksum, which
  // makes it a cheap determinism / equivalence check.
  std::uint64_t trade_checksum = 14695981039346656037ull;

  // Final book state, filled in by finalize().
  std::optional<Price> best_bid;
  std::optional<Price> best_ask;
  std::uint64_t resting_orders = 0;
  std::uint64_t bid_levels = 0;
  std::uint64_t ask_levels = 0;

  bool operator==(const ReplayStats&) const = default;
};

// Apply one message to the book and fold the outcome into `stats`.
void apply(OrderBook& book, const Message& m, ReplayStats& stats);

// Copy the final book state into `stats`.
void finalize(const OrderBook& book, ReplayStats& stats);

struct ReplayResult {
  ReplayStats stats;
  DecodeStatus decode_status = DecodeStatus::Ok;  // Ok means the whole file parsed
  bool header_ok = true;
};

// Decode a feed file image and run it through a fresh book on this thread.
ReplayResult replay_bytes(std::span<const std::uint8_t> file);

#ifndef __EMSCRIPTEN__  // not available in the single-threaded WebAssembly build
// Same result, but decoding runs on its own thread and hands messages to the
// book thread (this one) through an SpscQueue of the given capacity.
ReplayResult replay_bytes_threaded(std::span<const std::uint8_t> file,
                                   std::size_t queue_capacity = 1 << 14);
#endif

}  // namespace lob
