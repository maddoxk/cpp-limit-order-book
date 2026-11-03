// A deliberately simple, obviously-correct order book used as the oracle in
// the randomized property test. It trades speed for clarity:
//   std::map<Price, std::deque<Order>> per side, linear search for cancel.
// It shares NO code with lob::OrderBook, only the public value types.
#pragma once

#include <algorithm>
#include <deque>
#include <iterator>
#include <map>
#include <optional>
#include <vector>

#include "lob/order_book.hpp"
#include "lob/types.hpp"

namespace lob::test {

class ReferenceBook {
 public:
  Result add_limit(OrderId id, Side side, Price price, Qty qty) {
    trades_.clear();
    if (id == 0) return {Status::InvalidId, 0, 0};
    if (price <= 0) return {Status::InvalidPrice, 0, 0};
    if (qty == 0) return {Status::InvalidQty, 0, 0};
    if (exists(id)) return {Status::DuplicateId, 0, 0};
    return enter(id, side, price, qty, /*market=*/false);
  }

  Result add_market(OrderId id, Side side, Qty qty) {
    trades_.clear();
    if (id == 0) return {Status::InvalidId, 0, 0};
    if (qty == 0) return {Status::InvalidQty, 0, 0};
    if (exists(id)) return {Status::DuplicateId, 0, 0};
    return enter(id, side, 0, qty, /*market=*/true);
  }

  Status cancel(OrderId id) {
    trades_.clear();
    return erase(id) ? Status::Ok : Status::UnknownId;
  }

  Result modify(OrderId id, Price new_price, Qty new_qty) {
    trades_.clear();
    if (new_price <= 0) return {Status::InvalidPrice, 0, 0};
    if (new_qty == 0) return {Status::InvalidQty, 0, 0};
    Order* o = locate(id);
    if (o == nullptr) return {Status::UnknownId, 0, 0};
    if (new_price == cur_price_ && new_qty <= o->qty) {
      o->qty = new_qty;  // in place: same position in the deque
      return {Status::Ok, 0, new_qty};
    }
    const Side side = cur_side_;
    erase(id);
    return enter(id, side, new_price, new_qty, /*market=*/false);
  }

  Status execute(OrderId id, Qty qty) {
    trades_.clear();
    if (qty == 0) return Status::InvalidQty;
    Order* o = locate(id);
    if (o == nullptr) return Status::UnknownId;
    if (qty > o->qty) return Status::InvalidQty;
    trades_.push_back(
        Trade{id, kExternalTaker, cur_price_, qty, opposite(cur_side_)});
    if (qty == o->qty) {
      erase(id);
    } else {
      o->qty -= qty;
    }
    return Status::Ok;
  }

  std::optional<Price> best_bid() const {
    if (bids_.empty()) return std::nullopt;
    return bids_.rbegin()->first;
  }
  std::optional<Price> best_ask() const {
    if (asks_.empty()) return std::nullopt;
    return asks_.begin()->first;
  }

  std::vector<DepthLevel> depth(Side side) const {
    std::vector<DepthLevel> out;
    auto add = [&out](Price p, const std::deque<Order>& q) {
      std::uint64_t total = 0;
      for (const Order& o : q) total += o.qty;
      out.push_back(DepthLevel{p, total, static_cast<std::uint32_t>(q.size())});
    };
    if (side == Side::Buy) {
      for (auto it = bids_.rbegin(); it != bids_.rend(); ++it) add(it->first, it->second);
    } else {
      for (const auto& [p, q] : asks_) add(p, q);
    }
    return out;
  }

  std::size_t order_count() const {
    std::size_t n = 0;
    for (const auto& [p, q] : bids_) n += q.size();
    for (const auto& [p, q] : asks_) n += q.size();
    return n;
  }

  const std::vector<Trade>& last_trades() const { return trades_; }

 private:
  struct Order {
    OrderId id;
    Qty qty;
  };
  using SideMap = std::map<Price, std::deque<Order>>;

  bool exists(OrderId id) { return locate(id) != nullptr; }

  // Linear scan; also records the order's side and price in cur_*.
  Order* locate(OrderId id) {
    for (auto* m : {&bids_, &asks_}) {
      for (auto& [price, q] : *m) {
        for (Order& o : q) {
          if (o.id == id) {
            cur_side_ = (m == &bids_) ? Side::Buy : Side::Sell;
            cur_price_ = price;
            return &o;
          }
        }
      }
    }
    return nullptr;
  }

  bool erase(OrderId id) {
    for (auto* m : {&bids_, &asks_}) {
      for (auto it = m->begin(); it != m->end(); ++it) {
        auto& q = it->second;
        for (auto oi = q.begin(); oi != q.end(); ++oi) {
          if (oi->id == id) {
            q.erase(oi);
            if (q.empty()) m->erase(it);
            return true;
          }
        }
      }
    }
    return false;
  }

  Result enter(OrderId id, Side side, Price price, Qty qty, bool market) {
    Qty remaining = qty;
    SideMap& opp = side == Side::Buy ? asks_ : bids_;
    while (remaining > 0 && !opp.empty()) {
      auto it = side == Side::Buy ? opp.begin() : std::prev(opp.end());
      const Price level_price = it->first;
      if (!market) {
        if (side == Side::Buy && level_price > price) break;
        if (side == Side::Sell && level_price < price) break;
      }
      auto& q = it->second;
      while (remaining > 0 && !q.empty()) {
        Order& maker = q.front();
        const Qty fill = std::min(remaining, maker.qty);
        trades_.push_back(Trade{maker.id, id, level_price, fill, side});
        maker.qty -= fill;
        remaining -= fill;
        if (maker.qty == 0) q.pop_front();
      }
      if (q.empty()) opp.erase(it);
    }
    const Qty filled = qty - remaining;
    if (market) return {Status::Ok, filled, 0};
    if (remaining > 0) {
      (side == Side::Buy ? bids_ : asks_)[price].push_back(Order{id, remaining});
    }
    return {Status::Ok, filled, remaining};
  }

  SideMap bids_;
  SideMap asks_;
  std::vector<Trade> trades_;
  Side cur_side_ = Side::Buy;
  Price cur_price_ = 0;
};

}  // namespace lob::test
