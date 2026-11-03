// Single-symbol limit order book with a price-time priority matching engine.
//
// Data structures (see docs/DESIGN.md for the reasoning):
//   * Orders live in one contiguous pool (std::vector<Order>). They are linked
//     into per-price FIFO queues with 32-bit *indices*, not pointers, so the
//     pool can grow without invalidating links.
//   * Each side keeps a flat, sorted std::vector<Level>. The BEST price is at
//     the back, so touching the top of the book is O(1) and cache friendly.
//   * An unordered_map from order id to pool index gives O(1) cancel/modify.
//
// Not thread safe: one thread owns the book.
#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <unordered_map>
#include <vector>

#include "lob/types.hpp"

namespace lob {

// Outcome of an add / modify.
struct Result {
  Status status = Status::Ok;
  Qty filled = 0;   // quantity executed immediately against resting orders
  Qty resting = 0;  // quantity left resting in the book afterwards
};

// One price level in an L2 (aggregated by price) depth snapshot.
struct DepthLevel {
  Price price;
  std::uint64_t qty;     // total quantity at this price
  std::uint32_t orders;  // number of orders at this price

  bool operator==(const DepthLevel&) const = default;
};

// A copy of the state of one resting order.
struct OrderInfo {
  Side side;
  Price price;
  Qty qty;
};

class OrderBook {
 public:
  // expected_orders pre-sizes the pool and hash map to avoid rehashing.
  explicit OrderBook(std::size_t expected_orders = 0);

  // --- Order entry --------------------------------------------------------

  // Match against the opposite side while prices cross; rest any remainder.
  Result add_limit(OrderId id, Side side, Price price, Qty qty);

  // Match against the opposite side at any price. Any unfilled remainder is
  // discarded (a market order never rests).
  Result add_market(OrderId id, Side side, Qty qty);

  // Remove a resting order.
  Status cancel(OrderId id);

  // Change a resting order.
  //  * same price, smaller qty : reduced in place, KEEPS time priority
  //  * same price, same qty    : no-op, keeps priority
  //  * larger qty or new price : cancel + re-enter with the same id, LOSES
  //                              priority, and may match immediately
  // new_qty == 0 is rejected (use cancel).
  Result modify(OrderId id, Price new_price, Qty new_qty);

  // Report an execution of `qty` against a resting order (as an exchange feed
  // would). Reduces the order in place (keeps priority), removing it if it
  // reaches zero, and records a Trade with taker_id == kExternalTaker.
  // qty must be in [1, resting qty].
  Status execute(OrderId id, Qty qty);

  // --- Queries ------------------------------------------------------------

  std::optional<Price> best_bid() const noexcept;
  std::optional<Price> best_ask() const noexcept;

  // Top `max_levels` price levels of one side, best first.
  std::vector<DepthLevel> depth(Side side, std::size_t max_levels) const;

  std::optional<OrderInfo> find(OrderId id) const;
  bool contains(OrderId id) const { return index_.contains(id); }
  std::size_t order_count() const noexcept { return index_.size(); }
  std::size_t level_count(Side side) const noexcept {
    return side_of(side).levels.size();
  }

  // Trades produced by the most recent mutating call, in execution order.
  // The vector is cleared at the start of every mutating call.
  const std::vector<Trade>& last_trades() const noexcept { return trades_; }

 private:
  static constexpr std::uint32_t kNil = std::numeric_limits<std::uint32_t>::max();

  struct Order {
    OrderId id;
    Price price;
    Qty qty;
    Side side;
    std::uint32_t prev;  // neighbours in the price level's FIFO queue
    std::uint32_t next;  // (also used as the free-list link when unused)
  };

  struct Level {
    Price price;
    std::uint32_t count;      // orders in the queue
    std::uint32_t head;       // oldest order = first to be matched
    std::uint32_t tail;       // newest order
    std::uint64_t total_qty;  // sum of order quantities
  };

  struct BookSide {
    bool is_bid = false;
    // Sorted by key() ascending, so the best price is levels.back().
    std::vector<Level> levels;

    // Bids: higher price is better; asks: lower price is better. Negating
    // the ask price lets both sides share one "ascending by key" ordering.
    std::int64_t key(Price p) const noexcept {
      return is_bid ? std::int64_t{p} : -std::int64_t{p};
    }
    // Index of the first level whose key is >= key(price) (binary search).
    std::size_t lower_bound(Price price) const noexcept;
  };

  BookSide& side_of(Side s) noexcept { return s == Side::Buy ? bids_ : asks_; }
  const BookSide& side_of(Side s) const noexcept {
    return s == Side::Buy ? bids_ : asks_;
  }

  // Core routines (validation already done by the callers).
  Result submit_limit(OrderId id, Side side, Price price, Qty qty);
  Qty match(OrderId taker_id, Side taker_side, bool is_market, Price limit, Qty qty);
  void rest(std::uint32_t idx);
  void reduce_in_place(std::uint32_t idx, Qty delta);
  void remove_order(std::uint32_t idx);

  std::uint32_t allocate(OrderId id, Side side, Price price, Qty qty);
  void release(std::uint32_t idx) noexcept;

  BookSide bids_;
  BookSide asks_;
  std::vector<Order> orders_;  // the pool
  std::uint32_t free_head_ = kNil;
  std::unordered_map<OrderId, std::uint32_t> index_;  // id -> pool index
  std::vector<Trade> trades_;
};

}  // namespace lob
