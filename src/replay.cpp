#include "lob/replay.hpp"

#ifndef __EMSCRIPTEN__
#include <atomic>
#include <thread>

#include "lob/spsc_queue.hpp"
#endif

namespace lob {
namespace {

// Rough size of a typical working set; just avoids early rehashing.
constexpr std::size_t kExpectedOrders = 1 << 16;

void mix(std::uint64_t& h, std::uint64_t v) {
  // FNV-1a, one byte at a time over the 8 bytes of v.
  for (int i = 0; i < 8; ++i) {
    h ^= (v >> (8 * i)) & 0xffu;
    h *= 1099511628211ull;
  }
}

}  // namespace

void apply(OrderBook& book, const Message& m, ReplayStats& stats) {
  ++stats.messages;
  Status status = Status::Ok;
  switch (m.type) {
    case MsgType::Add:
      if (m.order_type == OrderType::Market) {
        ++stats.market_orders;
        status = book.add_market(m.id, m.side, m.qty).status;
      } else {
        ++stats.limit_orders;
        status = book.add_limit(m.id, m.side, m.price, m.qty).status;
      }
      break;
    case MsgType::Cancel:
      ++stats.cancels;
      status = book.cancel(m.id);
      break;
    case MsgType::Modify:
      ++stats.modifies;
      status = book.modify(m.id, m.price, m.qty).status;
      break;
    case MsgType::Execute:
      ++stats.executes;
      status = book.execute(m.id, m.qty);
      break;
  }
  if (status != Status::Ok) ++stats.rejects;

  for (const Trade& t : book.last_trades()) {
    ++stats.trades;
    stats.traded_qty += t.qty;
    mix(stats.trade_checksum, t.maker_id);
    mix(stats.trade_checksum, t.taker_id);
    mix(stats.trade_checksum, static_cast<std::uint32_t>(t.price));
    mix(stats.trade_checksum, t.qty);
  }
}

void finalize(const OrderBook& book, ReplayStats& stats) {
  stats.best_bid = book.best_bid();
  stats.best_ask = book.best_ask();
  stats.resting_orders = book.order_count();
  stats.bid_levels = book.level_count(Side::Buy);
  stats.ask_levels = book.level_count(Side::Sell);
}

ReplayResult replay_bytes(std::span<const std::uint8_t> file) {
  ReplayResult result;
  if (!check_file_header(file)) {
    result.header_ok = false;
    return result;
  }
  OrderBook book(kExpectedOrders);
  std::span<const std::uint8_t> rest = file.subspan(kFileHeaderSize);
  while (!rest.empty()) {
    Message m;
    const DecodeResult d = decode(rest, m);
    if (d.status != DecodeStatus::Ok) {
      result.decode_status = d.status;
      break;
    }
    apply(book, m, result.stats);
    rest = rest.subspan(d.consumed);
  }
  finalize(book, result.stats);
  return result;
}

// The two-thread pipeline needs std::thread, which the WebAssembly demo build
// does not use (single-threaded wasm), so it is compiled out there.
#ifndef __EMSCRIPTEN__
ReplayResult replay_bytes_threaded(std::span<const std::uint8_t> file,
                                   std::size_t queue_capacity) {
  ReplayResult result;
  if (!check_file_header(file)) {
    result.header_ok = false;
    return result;
  }

  SpscQueue<Message> queue(queue_capacity);
  std::atomic<bool> producer_done{false};
  DecodeStatus decode_status = DecodeStatus::Ok;  // written by producer, read after join

  // Producer: the "feed handler" thread. Decodes bytes into Messages.
  std::thread producer([&] {
    std::span<const std::uint8_t> rest = file.subspan(kFileHeaderSize);
    while (!rest.empty()) {
      Message m;
      const DecodeResult d = decode(rest, m);
      if (d.status != DecodeStatus::Ok) {
        decode_status = d.status;
        break;
      }
      while (!queue.try_push(m)) std::this_thread::yield();  // queue full: wait
      rest = rest.subspan(d.consumed);
    }
    producer_done.store(true, std::memory_order_release);
  });

  // Consumer: this thread owns the book.
  OrderBook book(kExpectedOrders);
  Message m;
  for (;;) {
    if (queue.try_pop(m)) {
      apply(book, m, result.stats);
      continue;
    }
    if (producer_done.load(std::memory_order_acquire)) {
      // The producer may have pushed its last messages between our failed pop
      // and reading the flag, so drain once more before stopping.
      if (queue.try_pop(m)) {
        apply(book, m, result.stats);
        continue;
      }
      break;
    }
    std::this_thread::yield();  // queue empty: wait
  }
  producer.join();

  result.decode_status = decode_status;
  finalize(book, result.stats);
  return result;
}

#endif  // __EMSCRIPTEN__

}  // namespace lob
