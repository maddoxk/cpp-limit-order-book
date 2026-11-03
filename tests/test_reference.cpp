// Randomized property test: drive lob::OrderBook and the simple
// ReferenceBook with the same random operation stream and require identical
// behaviour (status, fills, trades, top of book, full depth) after every step.
#include <gtest/gtest.h>

#include <random>
#include <string>
#include <vector>

#include "lob/order_book.hpp"
#include "reference_book.hpp"

namespace {

using namespace lob;

void expect_same_depth(const OrderBook& book, const test::ReferenceBook& ref,
                       const std::string& ctx) {
  for (Side s : {Side::Buy, Side::Sell}) {
    EXPECT_EQ(book.depth(s, 1'000'000), ref.depth(s)) << ctx;
  }
  EXPECT_EQ(book.order_count(), ref.order_count()) << ctx;
}

void run_random(std::uint32_t seed, int ops, Price lo, Price hi) {
  std::mt19937 rng(seed);
  auto uni = [&rng](std::uint32_t n) { return static_cast<std::uint32_t>(rng() % n); };
  auto rand_price = [&] { return lo + static_cast<Price>(uni(static_cast<std::uint32_t>(hi - lo + 1))); };
  auto rand_qty = [&] { return 1 + uni(20); };

  OrderBook book;
  test::ReferenceBook ref;
  OrderId next_id = 1;
  std::vector<OrderId> issued;  // includes ids that are dead, to hit UnknownId

  auto pick_id = [&]() -> OrderId {
    if (issued.empty() || uni(20) == 0) return next_id + 1000;  // never issued
    return issued[uni(static_cast<std::uint32_t>(issued.size()))];
  };

  for (int i = 0; i < ops; ++i) {
    const std::string ctx = "seed=" + std::to_string(seed) + " op=" + std::to_string(i);
    const std::uint32_t kind = uni(100);

    if (kind < 40) {  // limit
      const OrderId id = next_id++;
      issued.push_back(id);
      const Side side = uni(2) ? Side::Buy : Side::Sell;
      const Price p = rand_price();
      const Qty q = rand_qty();
      const Result a = book.add_limit(id, side, p, q);
      const Result b = ref.add_limit(id, side, p, q);
      ASSERT_EQ(a.status, b.status) << ctx;
      ASSERT_EQ(a.filled, b.filled) << ctx;
      ASSERT_EQ(a.resting, b.resting) << ctx;
    } else if (kind < 50) {  // market
      const OrderId id = next_id++;
      const Side side = uni(2) ? Side::Buy : Side::Sell;
      const Qty q = rand_qty();
      const Result a = book.add_market(id, side, q);
      const Result b = ref.add_market(id, side, q);
      ASSERT_EQ(a.status, b.status) << ctx;
      ASSERT_EQ(a.filled, b.filled) << ctx;
    } else if (kind < 70) {  // cancel
      const OrderId id = pick_id();
      ASSERT_EQ(book.cancel(id), ref.cancel(id)) << ctx;
    } else if (kind < 90) {  // modify (sometimes same price, sometimes new)
      const OrderId id = pick_id();
      const Price p = rand_price();
      const Qty q = rand_qty();
      const Result a = book.modify(id, p, q);
      const Result b = ref.modify(id, p, q);
      ASSERT_EQ(a.status, b.status) << ctx;
      ASSERT_EQ(a.filled, b.filled) << ctx;
      ASSERT_EQ(a.resting, b.resting) << ctx;
    } else if (kind < 97) {  // execute
      const OrderId id = pick_id();
      const Qty q = 1 + uni(25);  // sometimes larger than the resting qty
      ASSERT_EQ(book.execute(id, q), ref.execute(id, q)) << ctx;
    } else {  // invalid inputs
      const OrderId id = next_id++;
      ASSERT_EQ(book.add_limit(id, Side::Buy, 0, 5).status,
                ref.add_limit(id, Side::Buy, 0, 5).status) << ctx;
      ASSERT_EQ(book.add_limit(id, Side::Sell, 10, 0).status,
                ref.add_limit(id, Side::Sell, 10, 0).status) << ctx;
    }

    ASSERT_EQ(book.last_trades(), ref.last_trades()) << ctx;
    ASSERT_EQ(book.best_bid(), ref.best_bid()) << ctx;
    ASSERT_EQ(book.best_ask(), ref.best_ask()) << ctx;
    if (i % 25 == 0) expect_same_depth(book, ref, ctx);
    // The book must never be left crossed.
    if (book.best_bid() && book.best_ask()) {
      ASSERT_LT(*book.best_bid(), *book.best_ask()) << ctx;
    }
  }
  expect_same_depth(book, ref, "final seed=" + std::to_string(seed));
}

TEST(PropertyTest, MatchesReferenceNarrowPriceRange) {
  // Few distinct prices => lots of crossing, deep queues, partial fills.
  for (std::uint32_t seed = 1; seed <= 30; ++seed) run_random(seed, 4000, 98, 102);
}

TEST(PropertyTest, MatchesReferenceWidePriceRange) {
  // Many levels => exercises sorted-vector insert/erase in the middle.
  for (std::uint32_t seed = 100; seed < 110; ++seed) run_random(seed, 4000, 1, 200);
}

}  // namespace
