// Thin extern "C" binding over the existing C++ engine, for JavaScript.
//
// No matching logic lives here: every call forwards to lob::OrderBook,
// lob::FeedGenerator or lob::apply(). The only job of this file is to
// (a) hold one global book, (b) keep a ring buffer of recent trades for the
// tape, and (c) hand results to JS as plain numbers.
//
// Conventions:
//  * Order ids cross the boundary as doubles (exact up to 2^53) so we do not
//    need BigInt support.
//  * Variable-size results (depth, trades) are written into a static double
//    buffer; JS reads it element by element with lob_out(i).
//  * side: 0 = buy, 1 = sell. Status codes are lob::Status values
//    (0 Ok, 1 InvalidId, 2 InvalidPrice, 3 InvalidQty, 4 DuplicateId, 5 UnknownId).
#include <emscripten/emscripten.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <vector>

#include "lob/generator.hpp"
#include "lob/order_book.hpp"
#include "lob/replay.hpp"

namespace {

using namespace lob;

constexpr std::size_t kTapeSize = 1024;  // recent trades kept for the tape
constexpr std::size_t kOutSize = 4096;

struct TapeEntry {
  std::uint64_t seq;
  Trade trade;
};

struct State {
  OrderBook book{1 << 16};
  std::unique_ptr<FeedGenerator> gen;
  ReplayStats stream_stats;  // counts for messages driven by the synthetic stream
  TapeEntry tape[kTapeSize] = {};
  std::uint64_t trade_seq = 0;
  std::uint64_t user_ops = 0;
  Qty last_filled = 0;
  Qty last_resting = 0;
};

std::unique_ptr<State> g;
double g_out[kOutSize];
double g_bench[8];

State& st() {
  if (!g) {
    g = std::make_unique<State>();
    g->gen = std::make_unique<FeedGenerator>(GeneratorConfig{});
  }
  return *g;
}

Side to_side(int s) { return s == 0 ? Side::Buy : Side::Sell; }

void record_trades(State& s) {
  for (const Trade& t : s.book.last_trades()) {
    ++s.trade_seq;
    s.tape[s.trade_seq % kTapeSize] = TapeEntry{s.trade_seq, t};
  }
}

int finish(State& s, const Result& r) {
  ++s.user_ops;
  s.last_filled = r.filled;
  s.last_resting = r.resting;
  record_trades(s);
  return static_cast<int>(r.status);
}

}  // namespace

extern "C" {

// Start over with an empty book and a generator seeded with `seed`.
EMSCRIPTEN_KEEPALIVE void lob_reset(unsigned seed) {
  g = std::make_unique<State>();
  GeneratorConfig cfg;
  cfg.seed = seed;
  g->gen = std::make_unique<FeedGenerator>(cfg);
}

// --- order entry -----------------------------------------------------------

EMSCRIPTEN_KEEPALIVE int lob_limit(double id, int side, int price, unsigned qty) {
  State& s = st();
  return finish(s, s.book.add_limit(static_cast<OrderId>(id), to_side(side), price, qty));
}

EMSCRIPTEN_KEEPALIVE int lob_market(double id, int side, unsigned qty) {
  State& s = st();
  return finish(s, s.book.add_market(static_cast<OrderId>(id), to_side(side), qty));
}

EMSCRIPTEN_KEEPALIVE int lob_cancel(double id) {
  State& s = st();
  ++s.user_ops;
  s.last_filled = s.last_resting = 0;
  const Status r = s.book.cancel(static_cast<OrderId>(id));
  record_trades(s);
  return static_cast<int>(r);
}

EMSCRIPTEN_KEEPALIVE int lob_modify(double id, int price, unsigned qty) {
  State& s = st();
  return finish(s, s.book.modify(static_cast<OrderId>(id), price, qty));
}

// Fill/rest quantities of the most recent limit/market/modify call.
EMSCRIPTEN_KEEPALIVE double lob_last_filled() { return st().last_filled; }
EMSCRIPTEN_KEEPALIVE double lob_last_resting() { return st().last_resting; }

// --- queries ---------------------------------------------------------------

// Reads out[i]: result of the latest lob_depth / lob_trades_fetch / lob_order.
EMSCRIPTEN_KEEPALIVE double lob_out(int i) { return g_out[i]; }

// Top n levels of a side, best first. Writes [price, qty, orders] triples.
// Returns the number of levels.
EMSCRIPTEN_KEEPALIVE int lob_depth(int side, int n) {
  if (n > 100) n = 100;
  const auto levels = st().book.depth(to_side(side), static_cast<std::size_t>(n));
  int k = 0;
  for (const DepthLevel& d : levels) {
    g_out[k++] = d.price;
    g_out[k++] = static_cast<double>(d.qty);
    g_out[k++] = d.orders;
  }
  return static_cast<int>(levels.size());
}

// Trades with seq > after_seq (at most `max`, newest ones if more), oldest
// first, as [seq, price, qty, taker_side, maker_id, taker_id] sextuples.
// Returns the count. JS remembers the last seq it saw.
EMSCRIPTEN_KEEPALIVE int lob_trades_fetch(double after_seq, int max) {
  State& s = st();
  if (max > 200) max = 200;
  std::uint64_t first = after_seq < 0 ? 1 : static_cast<std::uint64_t>(after_seq) + 1;
  if (s.trade_seq >= kTapeSize && first <= s.trade_seq - kTapeSize) {
    first = s.trade_seq - kTapeSize + 1;  // older entries were overwritten
  }
  if (s.trade_seq >= static_cast<std::uint64_t>(max) &&
      first + static_cast<std::uint64_t>(max) <= s.trade_seq) {
    first = s.trade_seq - static_cast<std::uint64_t>(max) + 1;
  }
  int k = 0;
  int count = 0;
  for (std::uint64_t q = first; q <= s.trade_seq; ++q) {
    const TapeEntry& e = s.tape[q % kTapeSize];
    g_out[k++] = static_cast<double>(e.seq);
    g_out[k++] = e.trade.price;
    g_out[k++] = e.trade.qty;
    g_out[k++] = e.trade.taker_side == Side::Buy ? 0 : 1;
    g_out[k++] = static_cast<double>(e.trade.maker_id);
    g_out[k++] = static_cast<double>(e.trade.taker_id);
    ++count;
  }
  return count;
}

// Resting order lookup: returns 1 and writes [side, price, qty], or 0.
EMSCRIPTEN_KEEPALIVE int lob_order(double id) {
  const auto o = st().book.find(static_cast<OrderId>(id));
  if (!o) return 0;
  g_out[0] = o->side == Side::Buy ? 0 : 1;
  g_out[1] = o->price;
  g_out[2] = o->qty;
  return 1;
}

// 0 stream msgs, 1 trades (all sources), 2 resting orders, 3 best bid (0=none),
// 4 best ask (0=none), 5 bid levels, 6 ask levels, 7 stream rejects, 8 user ops.
EMSCRIPTEN_KEEPALIVE double lob_stat(int k) {
  State& s = st();
  switch (k) {
    case 0: return static_cast<double>(s.stream_stats.messages);
    case 1: return static_cast<double>(s.trade_seq);
    case 2: return static_cast<double>(s.book.order_count());
    case 3: return s.book.best_bid().value_or(0);
    case 4: return s.book.best_ask().value_or(0);
    case 5: return static_cast<double>(s.book.level_count(Side::Buy));
    case 6: return static_cast<double>(s.book.level_count(Side::Sell));
    case 7: return static_cast<double>(s.stream_stats.rejects);
    case 8: return static_cast<double>(s.user_ops);
  }
  return 0;
}

// --- synthetic feed --------------------------------------------------------

// Generate n messages with the existing FeedGenerator and apply them to the
// live book (so the ladder and tape animate). Returns n.
EMSCRIPTEN_KEEPALIVE int lob_stream_step(int n) {
  State& s = st();
  for (int i = 0; i < n; ++i) {
    const Message m = s.gen->next();
    apply(s.book, m, s.stream_stats);
    record_trades(s);
  }
  return n;
}

// Replay n synthetic messages (given seed) into a fresh private book and time
// ONLY the apply loop with a monotonic clock; generation is done beforehand.
// Returns elapsed milliseconds. Details via lob_bench_stat:
// 0 messages, 1 trades, 2 rejects, 3 trade checksum (low 32 bits), 4 resting orders.
EMSCRIPTEN_KEEPALIVE double lob_bench(int n, unsigned seed) {
  GeneratorConfig cfg;
  cfg.seed = seed;
  const std::vector<Message> msgs = FeedGenerator(cfg).generate(static_cast<std::size_t>(n));

  OrderBook book(1 << 16);
  ReplayStats stats;
  const auto t0 = std::chrono::steady_clock::now();
  for (const Message& m : msgs) apply(book, m, stats);
  const auto t1 = std::chrono::steady_clock::now();
  finalize(book, stats);

  g_bench[0] = static_cast<double>(stats.messages);
  g_bench[1] = static_cast<double>(stats.trades);
  g_bench[2] = static_cast<double>(stats.rejects);
  g_bench[3] = static_cast<double>(stats.trade_checksum & 0xffffffffull);
  g_bench[4] = static_cast<double>(stats.resting_orders);
  return std::chrono::duration<double, std::milli>(t1 - t0).count();
}

EMSCRIPTEN_KEEPALIVE double lob_bench_stat(int k) { return g_bench[k]; }

}  // extern "C"
