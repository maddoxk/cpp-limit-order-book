#include "lob/order_book.hpp"

#include <algorithm>
#include <cassert>

namespace lob {

// ---------------------------------------------------------------------------
// BookSide
// ---------------------------------------------------------------------------

std::size_t OrderBook::BookSide::lower_bound(Price price) const noexcept {
  const std::int64_t k = key(price);
  std::size_t lo = 0;
  std::size_t hi = levels.size();
  while (lo < hi) {
    const std::size_t mid = lo + (hi - lo) / 2;
    if (key(levels[mid].price) < k) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  return lo;
}

// ---------------------------------------------------------------------------
// Construction and the order pool
// ---------------------------------------------------------------------------

OrderBook::OrderBook(std::size_t expected_orders) {
  bids_.is_bid = true;
  asks_.is_bid = false;
  if (expected_orders > 0) {
    orders_.reserve(expected_orders);
    index_.reserve(expected_orders);
  }
}

std::uint32_t OrderBook::allocate(OrderId id, Side side, Price price, Qty qty) {
  const Order o{id, price, qty, side, kNil, kNil};
  if (free_head_ != kNil) {
    // Reuse a slot from the free list (its `next` field links free slots).
    const std::uint32_t idx = free_head_;
    free_head_ = orders_[idx].next;
    orders_[idx] = o;
    return idx;
  }
  orders_.push_back(o);
  return static_cast<std::uint32_t>(orders_.size() - 1);
}

void OrderBook::release(std::uint32_t idx) noexcept {
  orders_[idx].next = free_head_;
  free_head_ = idx;
}

// ---------------------------------------------------------------------------
// Resting and removing orders
// ---------------------------------------------------------------------------

// Append the order at the tail of its price level's FIFO (newest = last in
// line), creating the level if needed.
void OrderBook::rest(std::uint32_t idx) {
  Order& o = orders_[idx];
  BookSide& s = side_of(o.side);
  const std::size_t i = s.lower_bound(o.price);
  if (i == s.levels.size() || s.levels[i].price != o.price) {
    s.levels.insert(s.levels.begin() + static_cast<std::ptrdiff_t>(i),
                    Level{o.price, 0, kNil, kNil, 0});
  }
  Level& level = s.levels[i];
  o.prev = level.tail;
  o.next = kNil;
  if (level.tail != kNil) {
    orders_[level.tail].next = idx;
  } else {
    level.head = idx;
  }
  level.tail = idx;
  ++level.count;
  level.total_qty += o.qty;
  index_.emplace(o.id, idx);
}

// Shrink a resting order without moving it in the queue (keeps priority).
// Caller guarantees 0 < delta < order qty.
void OrderBook::reduce_in_place(std::uint32_t idx, Qty delta) {
  Order& o = orders_[idx];
  BookSide& s = side_of(o.side);
  const std::size_t i = s.lower_bound(o.price);
  assert(i < s.levels.size() && s.levels[i].price == o.price);
  o.qty -= delta;
  s.levels[i].total_qty -= delta;
}

// Unlink an order from its level (erasing the level if it becomes empty),
// drop it from the id index and return its slot to the pool.
void OrderBook::remove_order(std::uint32_t idx) {
  Order& o = orders_[idx];
  BookSide& s = side_of(o.side);
  const std::size_t i = s.lower_bound(o.price);
  assert(i < s.levels.size() && s.levels[i].price == o.price);
  Level& level = s.levels[i];

  if (o.prev != kNil) {
    orders_[o.prev].next = o.next;
  } else {
    level.head = o.next;
  }
  if (o.next != kNil) {
    orders_[o.next].prev = o.prev;
  } else {
    level.tail = o.prev;
  }
  level.total_qty -= o.qty;
  --level.count;
  if (level.count == 0) {
    s.levels.erase(s.levels.begin() + static_cast<std::ptrdiff_t>(i));
  }
  index_.erase(o.id);
  release(idx);
}

// ---------------------------------------------------------------------------
// Matching
// ---------------------------------------------------------------------------

// Walk the opposite side from the best price. Within a level, fill the oldest
// order first (time priority). Returns the quantity filled.
Qty OrderBook::match(OrderId taker_id, Side taker_side, bool is_market,
                     Price limit, Qty qty) {
  BookSide& opp = side_of(opposite(taker_side));
  Qty filled = 0;

  while (qty > 0 && !opp.levels.empty()) {
    Level& level = opp.levels.back();  // best opposite level
    if (!is_market) {
      // A buy crosses while limit >= ask; a sell while limit <= bid.
      const bool crosses = taker_side == Side::Buy ? limit >= level.price
                                                   : limit <= level.price;
      if (!crosses) break;
    }

    while (qty > 0 && level.head != kNil) {
      const std::uint32_t idx = level.head;
      Order& maker = orders_[idx];
      const Qty q = std::min(qty, maker.qty);
      trades_.push_back(Trade{maker.id, taker_id, level.price, q, taker_side});
      maker.qty -= q;
      level.total_qty -= q;
      qty -= q;
      filled += q;

      if (maker.qty == 0) {
        // Maker fully filled: pop it off the front of the queue.
        const std::uint32_t next = maker.next;
        index_.erase(maker.id);
        level.head = next;
        if (next != kNil) {
          orders_[next].prev = kNil;
        } else {
          level.tail = kNil;
        }
        --level.count;
        release(idx);  // note: `maker` must not be used after this line
      }
    }
    if (level.count == 0) {
      opp.levels.pop_back();  // `level` must not be used after this line
    }
  }
  return filled;
}

Result OrderBook::submit_limit(OrderId id, Side side, Price price, Qty qty) {
  const Qty filled = match(id, side, /*is_market=*/false, price, qty);
  const Qty remaining = qty - filled;
  if (remaining > 0) {
    rest(allocate(id, side, price, remaining));
  }
  return Result{Status::Ok, filled, remaining};
}

// ---------------------------------------------------------------------------
// Public order entry
// ---------------------------------------------------------------------------

Result OrderBook::add_limit(OrderId id, Side side, Price price, Qty qty) {
  trades_.clear();
  if (id == 0) return {Status::InvalidId, 0, 0};
  if (price <= 0) return {Status::InvalidPrice, 0, 0};
  if (qty == 0) return {Status::InvalidQty, 0, 0};
  if (index_.contains(id)) return {Status::DuplicateId, 0, 0};
  return submit_limit(id, side, price, qty);
}

Result OrderBook::add_market(OrderId id, Side side, Qty qty) {
  trades_.clear();
  if (id == 0) return {Status::InvalidId, 0, 0};
  if (qty == 0) return {Status::InvalidQty, 0, 0};
  if (index_.contains(id)) return {Status::DuplicateId, 0, 0};
  const Qty filled = match(id, side, /*is_market=*/true, 0, qty);
  return Result{Status::Ok, filled, 0};
}

Status OrderBook::cancel(OrderId id) {
  trades_.clear();
  const auto it = index_.find(id);
  if (it == index_.end()) return Status::UnknownId;
  remove_order(it->second);
  return Status::Ok;
}

Result OrderBook::modify(OrderId id, Price new_price, Qty new_qty) {
  trades_.clear();
  if (new_price <= 0) return {Status::InvalidPrice, 0, 0};
  if (new_qty == 0) return {Status::InvalidQty, 0, 0};
  const auto it = index_.find(id);
  if (it == index_.end()) return {Status::UnknownId, 0, 0};

  const std::uint32_t idx = it->second;
  const Order& o = orders_[idx];

  if (new_price == o.price && new_qty <= o.qty) {
    // Reducing size (or no change) never hurts anyone queued behind us, so
    // the order keeps its place in line.
    if (new_qty < o.qty) reduce_in_place(idx, o.qty - new_qty);
    return {Status::Ok, 0, new_qty};
  }

  // Price change or size increase: treat as cancel + new order. It goes to
  // the back of the queue and may match if the new price crosses the book.
  const Side side = o.side;
  remove_order(idx);
  return submit_limit(id, side, new_price, new_qty);
}

Status OrderBook::execute(OrderId id, Qty qty) {
  trades_.clear();
  if (qty == 0) return Status::InvalidQty;
  const auto it = index_.find(id);
  if (it == index_.end()) return Status::UnknownId;

  const std::uint32_t idx = it->second;
  const Order& o = orders_[idx];
  if (qty > o.qty) return Status::InvalidQty;

  const Side taker_side = opposite(o.side);
  trades_.push_back(Trade{id, kExternalTaker, o.price, qty, taker_side});
  if (qty == o.qty) {
    remove_order(idx);
  } else {
    reduce_in_place(idx, qty);
  }
  return Status::Ok;
}

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------

std::optional<Price> OrderBook::best_bid() const noexcept {
  if (bids_.levels.empty()) return std::nullopt;
  return bids_.levels.back().price;
}

std::optional<Price> OrderBook::best_ask() const noexcept {
  if (asks_.levels.empty()) return std::nullopt;
  return asks_.levels.back().price;
}

std::vector<DepthLevel> OrderBook::depth(Side side, std::size_t max_levels) const {
  const BookSide& s = side_of(side);
  std::vector<DepthLevel> out;
  const std::size_t n = std::min(max_levels, s.levels.size());
  out.reserve(n);
  for (std::size_t k = 0; k < n; ++k) {
    const Level& level = s.levels[s.levels.size() - 1 - k];  // best first
    out.push_back(DepthLevel{level.price, level.total_qty, level.count});
  }
  return out;
}

std::optional<OrderInfo> OrderBook::find(OrderId id) const {
  const auto it = index_.find(id);
  if (it == index_.end()) return std::nullopt;
  const Order& o = orders_[it->second];
  return OrderInfo{o.side, o.price, o.qty};
}

}  // namespace lob
