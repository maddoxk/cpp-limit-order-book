#include "lob/generator.hpp"

#include <algorithm>
#include <cmath>

namespace lob {

FeedGenerator::FeedGenerator(const GeneratorConfig& cfg)
    : cfg_(cfg), rng_(cfg.seed), shadow_(1 << 14), mid_(cfg.start_mid) {}

double FeedGenerator::unit() {
  // Top 53 bits -> [0, 1) with full double precision.
  return static_cast<double>(rand64() >> 11) * (1.0 / 9007199254740992.0);
}

std::uint32_t FeedGenerator::expo(double mean) {
  const double x = -std::log(1.0 - unit()) * mean;
  return static_cast<std::uint32_t>(std::min(x, 1.0e6));
}

Message FeedGenerator::make_limit() {
  const OrderId id = next_id_++;
  const Side side = random_side();
  const bool aggressive = produced_ >= cfg_.warmup && unit() < cfg_.p_aggressive;
  Price price;
  if (aggressive) {
    // Priced through the mid, so it will usually cross the spread.
    const Price d = static_cast<Price>(expo(2.0));
    price = side == Side::Buy ? mid_ + d : mid_ - d;
  } else {
    // Passive: buys below the mid, sells above it.
    const Price d = 1 + static_cast<Price>(expo(cfg_.mean_offset_ticks));
    price = side == Side::Buy ? mid_ - d : mid_ + d;
  }
  price = std::max<Price>(price, 1);
  const Qty qty = 1 + std::min<Qty>(expo(cfg_.mean_qty), 10'000);
  return make_add(id, side, price, qty);
}

Message FeedGenerator::make_market_order() {
  const Qty qty = 1 + std::min<Qty>(expo(cfg_.mean_qty), 10'000);
  return make_market(next_id_++, random_side(), qty);
}

bool FeedGenerator::pick_live(OrderId& id, bool remove_from_live) {
  while (!live_.empty()) {
    const std::size_t i = below(static_cast<std::uint32_t>(live_.size()));
    const OrderId candidate = live_[i];
    const bool alive = shadow_.contains(candidate);
    if (!alive || remove_from_live) {
      live_[i] = live_.back();  // swap-remove: O(1), order of live_ is irrelevant
      live_.pop_back();
    }
    if (alive) {
      id = candidate;
      return true;
    }
  }
  return false;
}

Message FeedGenerator::next() {
  // Slow random walk of the mid price.
  if (unit() < 0.10) mid_ = std::max<Price>(mid_ + (below(2) ? 1 : -1), 50);

  Message m;
  const double total = cfg_.w_limit + cfg_.w_market + cfg_.w_cancel +
                       cfg_.w_modify + cfg_.w_execute;
  const double r = unit() * total;
  OrderId id = 0;

  if (produced_ < cfg_.warmup || r < cfg_.w_limit) {
    m = make_limit();
  } else if (r < cfg_.w_limit + cfg_.w_market) {
    m = make_market_order();
  } else if (r < cfg_.w_limit + cfg_.w_market + cfg_.w_cancel) {
    m = pick_live(id, /*remove_from_live=*/true) ? make_cancel(id) : make_limit();
  } else if (r < cfg_.w_limit + cfg_.w_market + cfg_.w_cancel + cfg_.w_modify) {
    if (pick_live(id, /*remove_from_live=*/false)) {
      const OrderInfo info = *shadow_.find(id);
      if (below(2) == 0) {
        // Reduce size at the same price (keeps queue priority).
        m = make_modify(id, info.price, 1 + below(info.qty));
      } else {
        // Re-price by up to 3 ticks either way (loses priority).
        const Price np = std::max<Price>(info.price + static_cast<Price>(below(7)) - 3, 1);
        m = make_modify(id, np, 1 + below(info.qty + 10));
      }
    } else {
      m = make_limit();
    }
  } else {
    m = pick_live(id, /*remove_from_live=*/false)
            ? make_execute(id, 1 + below(shadow_.find(id)->qty))
            : make_limit();
  }

  // Keep the shadow book in sync and remember newly resting orders.
  apply(shadow_, m, shadow_stats_);
  if (m.type == MsgType::Add && m.order_type == OrderType::Limit &&
      shadow_.contains(m.id)) {
    live_.push_back(m.id);
  }
  ++produced_;
  return m;
}

std::vector<Message> FeedGenerator::generate(std::size_t count) {
  std::vector<Message> out;
  out.reserve(count);
  for (std::size_t i = 0; i < count; ++i) out.push_back(next());
  return out;
}

}  // namespace lob
