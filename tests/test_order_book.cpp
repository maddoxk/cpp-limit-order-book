#include <gtest/gtest.h>

#include <vector>

#include "lob/order_book.hpp"

namespace {

using namespace lob;

constexpr Side B = Side::Buy;
constexpr Side S = Side::Sell;

// --- Basics ---------------------------------------------------------------

TEST(Book, EmptyBookHasNoTopOfBook) {
  OrderBook book;
  EXPECT_FALSE(book.best_bid());
  EXPECT_FALSE(book.best_ask());
  EXPECT_EQ(book.order_count(), 0u);
  EXPECT_TRUE(book.depth(B, 5).empty());
}

TEST(Book, NonCrossingOrdersRestAndSetTopOfBook) {
  OrderBook book;
  EXPECT_EQ(book.add_limit(1, B, 100, 10).resting, 10u);
  EXPECT_EQ(book.add_limit(2, B, 99, 5).resting, 5u);
  EXPECT_EQ(book.add_limit(3, S, 102, 7).resting, 7u);
  EXPECT_EQ(book.add_limit(4, S, 101, 3).resting, 3u);
  EXPECT_TRUE(book.last_trades().empty());
  EXPECT_EQ(book.best_bid(), 100);
  EXPECT_EQ(book.best_ask(), 101);
  EXPECT_EQ(book.order_count(), 4u);
}

TEST(Book, DepthSnapshotIsAggregatedAndBestFirst) {
  OrderBook book;
  book.add_limit(1, B, 100, 10);
  book.add_limit(2, B, 100, 5);
  book.add_limit(3, B, 98, 7);
  book.add_limit(4, B, 99, 1);
  book.add_limit(5, S, 103, 4);
  book.add_limit(6, S, 101, 2);
  book.add_limit(7, S, 101, 3);

  const std::vector<DepthLevel> bids = {{100, 15, 2}, {99, 1, 1}, {98, 7, 1}};
  EXPECT_EQ(book.depth(B, 10), bids);
  const std::vector<DepthLevel> asks = {{101, 5, 2}, {103, 4, 1}};
  EXPECT_EQ(book.depth(S, 10), asks);

  // Truncation to the top N levels.
  const std::vector<DepthLevel> top2 = {{100, 15, 2}, {99, 1, 1}};
  EXPECT_EQ(book.depth(B, 2), top2);
}

// --- Matching -------------------------------------------------------------

TEST(Match, CrossingLimitTradesAtRestingPrice) {
  OrderBook book;
  book.add_limit(1, S, 100, 10);
  // Aggressive buy at 105 still trades at the maker's price, 100.
  const Result r = book.add_limit(2, B, 105, 4);
  EXPECT_EQ(r.filled, 4u);
  EXPECT_EQ(r.resting, 0u);
  ASSERT_EQ(book.last_trades().size(), 1u);
  EXPECT_EQ(book.last_trades()[0], (Trade{1, 2, 100, 4, B}));
  EXPECT_EQ(book.find(1)->qty, 6u);
  EXPECT_FALSE(book.contains(2));
}

TEST(Match, PartialFillRestsRemainderAtLimitPrice) {
  OrderBook book;
  book.add_limit(1, S, 100, 4);
  const Result r = book.add_limit(2, B, 101, 10);
  EXPECT_EQ(r.filled, 4u);
  EXPECT_EQ(r.resting, 6u);
  EXPECT_EQ(book.best_bid(), 101);
  EXPECT_FALSE(book.best_ask());
  EXPECT_EQ(book.find(2)->qty, 6u);
}

TEST(Match, SweepsMultipleLevelsBestPriceFirst) {
  OrderBook book;
  book.add_limit(1, S, 102, 5);
  book.add_limit(2, S, 100, 5);
  book.add_limit(3, S, 101, 5);
  const Result r = book.add_limit(4, B, 102, 12);
  EXPECT_EQ(r.filled, 12u);
  const std::vector<Trade> expected = {
      {2, 4, 100, 5, B}, {3, 4, 101, 5, B}, {1, 4, 102, 2, B}};
  EXPECT_EQ(book.last_trades(), expected);
  EXPECT_EQ(book.best_ask(), 102);
  EXPECT_EQ(book.find(1)->qty, 3u);
}

TEST(Match, LimitDoesNotCrossBeyondItsPrice) {
  OrderBook book;
  book.add_limit(1, S, 100, 5);
  book.add_limit(2, S, 103, 5);
  const Result r = book.add_limit(3, B, 101, 20);
  EXPECT_EQ(r.filled, 5u);
  EXPECT_EQ(r.resting, 15u);
  EXPECT_EQ(book.best_bid(), 101);
  EXPECT_EQ(book.best_ask(), 103);
}

TEST(Match, SellSideAggressorMirrorsBuySide) {
  OrderBook book;
  book.add_limit(1, B, 100, 5);
  book.add_limit(2, B, 99, 5);
  const Result r = book.add_limit(3, S, 99, 8);
  EXPECT_EQ(r.filled, 8u);
  const std::vector<Trade> expected = {{1, 3, 100, 5, S}, {2, 3, 99, 3, S}};
  EXPECT_EQ(book.last_trades(), expected);
  EXPECT_EQ(book.best_bid(), 99);
}

TEST(Match, MarketOrderSweepsAndDiscardsRemainder) {
  OrderBook book;
  book.add_limit(1, S, 100, 3);
  book.add_limit(2, S, 105, 3);
  const Result r = book.add_market(3, B, 10);
  EXPECT_EQ(r.status, Status::Ok);
  EXPECT_EQ(r.filled, 6u);
  EXPECT_EQ(r.resting, 0u);
  EXPECT_EQ(book.last_trades().size(), 2u);
  EXPECT_EQ(book.order_count(), 0u);  // remainder 4 is NOT resting
  EXPECT_FALSE(book.contains(3));
}

TEST(Match, MarketOrderOnEmptyBookDoesNothing) {
  OrderBook book;
  const Result r = book.add_market(1, S, 10);
  EXPECT_EQ(r.status, Status::Ok);
  EXPECT_EQ(r.filled, 0u);
  EXPECT_EQ(book.order_count(), 0u);
}

TEST(Match, FullyFilledMakerIsRemovedAndLevelErased) {
  OrderBook book;
  book.add_limit(1, S, 100, 5);
  book.add_limit(2, S, 101, 5);
  book.add_market(3, B, 5);
  EXPECT_FALSE(book.contains(1));
  EXPECT_EQ(book.level_count(S), 1u);
  EXPECT_EQ(book.best_ask(), 101);
}

// --- Time priority --------------------------------------------------------

TEST(Priority, EarlierOrderAtSamePriceFillsFirst) {
  OrderBook book;
  book.add_limit(1, S, 100, 5);
  book.add_limit(2, S, 100, 5);
  book.add_limit(3, S, 100, 5);
  book.add_market(4, B, 7);
  const std::vector<Trade> expected = {{1, 4, 100, 5, B}, {2, 4, 100, 2, B}};
  EXPECT_EQ(book.last_trades(), expected);
}

TEST(Priority, BetterPriceBeatsEarlierTime) {
  OrderBook book;
  book.add_limit(1, B, 100, 5);  // earlier but worse
  book.add_limit(2, B, 101, 5);  // later but better
  book.add_market(3, S, 5);
  ASSERT_EQ(book.last_trades().size(), 1u);
  EXPECT_EQ(book.last_trades()[0].maker_id, 2u);
}

TEST(Priority, ReducingQtyKeepsPlaceInQueue) {
  OrderBook book;
  book.add_limit(1, S, 100, 10);
  book.add_limit(2, S, 100, 10);
  EXPECT_EQ(book.modify(1, 100, 4).status, Status::Ok);
  EXPECT_EQ(book.find(1)->qty, 4u);
  book.add_market(3, B, 5);
  const std::vector<Trade> expected = {{1, 3, 100, 4, B}, {2, 3, 100, 1, B}};
  EXPECT_EQ(book.last_trades(), expected);
}

TEST(Priority, IncreasingQtyLosesPlaceInQueue) {
  OrderBook book;
  book.add_limit(1, S, 100, 5);
  book.add_limit(2, S, 100, 5);
  EXPECT_EQ(book.modify(1, 100, 8).status, Status::Ok);
  book.add_market(3, B, 5);
  ASSERT_EQ(book.last_trades().size(), 1u);
  EXPECT_EQ(book.last_trades()[0].maker_id, 2u);  // order 2 now first
}

TEST(Priority, ChangingPriceLosesPlaceEvenIfBackAtSameQty) {
  OrderBook book;
  book.add_limit(1, B, 100, 5);
  book.add_limit(2, B, 100, 5);
  EXPECT_EQ(book.modify(1, 99, 5).status, Status::Ok);
  EXPECT_EQ(book.modify(1, 100, 5).status, Status::Ok);  // back, but at the tail
  book.add_market(3, S, 5);
  ASSERT_EQ(book.last_trades().size(), 1u);
  EXPECT_EQ(book.last_trades()[0].maker_id, 2u);
}

TEST(Priority, ModifyToSameValuesIsNoOpKeepingPriority) {
  OrderBook book;
  book.add_limit(1, S, 100, 5);
  book.add_limit(2, S, 100, 5);
  const Result r = book.modify(1, 100, 5);
  EXPECT_EQ(r.status, Status::Ok);
  EXPECT_EQ(r.filled, 0u);
  EXPECT_EQ(r.resting, 5u);
  book.add_market(3, B, 5);
  EXPECT_EQ(book.last_trades()[0].maker_id, 1u);
}

// --- Modify ---------------------------------------------------------------

TEST(Modify, RepricingAcrossTheSpreadMatchesImmediately) {
  OrderBook book;
  book.add_limit(1, S, 105, 5);
  book.add_limit(2, B, 100, 8);
  const Result r = book.modify(2, 105, 8);
  EXPECT_EQ(r.status, Status::Ok);
  EXPECT_EQ(r.filled, 5u);
  EXPECT_EQ(r.resting, 3u);
  const std::vector<Trade> expected = {{1, 2, 105, 5, B}};
  EXPECT_EQ(book.last_trades(), expected);
  EXPECT_EQ(book.best_bid(), 105);
  EXPECT_FALSE(book.best_ask());
}

TEST(Modify, UpdatesLevelAggregates) {
  OrderBook book;
  book.add_limit(1, B, 100, 10);
  book.add_limit(2, B, 100, 10);
  book.modify(1, 100, 3);
  EXPECT_EQ(book.depth(B, 1)[0], (DepthLevel{100, 13, 2}));
  book.modify(1, 101, 3);
  const std::vector<DepthLevel> expected = {{101, 3, 1}, {100, 10, 1}};
  EXPECT_EQ(book.depth(B, 5), expected);
}

TEST(Modify, RejectsBadInputsWithoutChangingBook) {
  OrderBook book;
  book.add_limit(1, B, 100, 10);
  EXPECT_EQ(book.modify(1, 100, 0).status, Status::InvalidQty);
  EXPECT_EQ(book.modify(1, 0, 10).status, Status::InvalidPrice);
  EXPECT_EQ(book.modify(1, -5, 10).status, Status::InvalidPrice);
  EXPECT_EQ(book.modify(99, 100, 10).status, Status::UnknownId);
  EXPECT_EQ(book.find(1)->qty, 10u);
  EXPECT_EQ(book.find(1)->price, 100);
}

TEST(Modify, FilledOrderCannotBeModified) {
  OrderBook book;
  book.add_limit(1, S, 100, 5);
  book.add_market(2, B, 5);
  EXPECT_EQ(book.modify(1, 100, 3).status, Status::UnknownId);
}

// --- Cancel ---------------------------------------------------------------

TEST(Cancel, RemovesOrderAndEmptyLevel) {
  OrderBook book;
  book.add_limit(1, B, 100, 5);
  book.add_limit(2, B, 99, 5);
  EXPECT_EQ(book.cancel(1), Status::Ok);
  EXPECT_EQ(book.best_bid(), 99);
  EXPECT_EQ(book.level_count(B), 1u);
  EXPECT_FALSE(book.contains(1));
}

TEST(Cancel, MiddleOfQueuePreservesOrderOfTheRest) {
  OrderBook book;
  book.add_limit(1, S, 100, 1);
  book.add_limit(2, S, 100, 1);
  book.add_limit(3, S, 100, 1);
  EXPECT_EQ(book.cancel(2), Status::Ok);
  book.add_market(4, B, 2);
  const std::vector<Trade> expected = {{1, 4, 100, 1, B}, {3, 4, 100, 1, B}};
  EXPECT_EQ(book.last_trades(), expected);
}

TEST(Cancel, HeadAndTailOfQueue) {
  OrderBook book;
  book.add_limit(1, S, 100, 1);
  book.add_limit(2, S, 100, 1);
  book.add_limit(3, S, 100, 1);
  EXPECT_EQ(book.cancel(1), Status::Ok);  // head
  EXPECT_EQ(book.cancel(3), Status::Ok);  // tail
  book.add_market(4, B, 5);
  ASSERT_EQ(book.last_trades().size(), 1u);
  EXPECT_EQ(book.last_trades()[0].maker_id, 2u);
}

TEST(Cancel, UnknownAndAlreadyCancelledAndFilled) {
  OrderBook book;
  EXPECT_EQ(book.cancel(1), Status::UnknownId);
  book.add_limit(1, B, 100, 5);
  EXPECT_EQ(book.cancel(1), Status::Ok);
  EXPECT_EQ(book.cancel(1), Status::UnknownId);
  book.add_limit(2, S, 100, 5);
  book.add_market(3, B, 5);
  EXPECT_EQ(book.cancel(2), Status::UnknownId);
}

TEST(Cancel, ClearsLastTrades) {
  OrderBook book;
  book.add_limit(1, S, 100, 5);
  book.add_market(2, B, 5);
  EXPECT_FALSE(book.last_trades().empty());
  book.cancel(99);
  EXPECT_TRUE(book.last_trades().empty());
}

// --- Validation -----------------------------------------------------------

TEST(Validation, RejectsBadAdds) {
  OrderBook book;
  EXPECT_EQ(book.add_limit(0, B, 100, 5).status, Status::InvalidId);
  EXPECT_EQ(book.add_limit(1, B, 0, 5).status, Status::InvalidPrice);
  EXPECT_EQ(book.add_limit(1, B, -3, 5).status, Status::InvalidPrice);
  EXPECT_EQ(book.add_limit(1, B, 100, 0).status, Status::InvalidQty);
  EXPECT_EQ(book.add_market(0, B, 5).status, Status::InvalidId);
  EXPECT_EQ(book.add_market(1, B, 0).status, Status::InvalidQty);
  EXPECT_EQ(book.order_count(), 0u);
}

TEST(Validation, DuplicateIdIsRejected) {
  OrderBook book;
  book.add_limit(1, B, 100, 5);
  EXPECT_EQ(book.add_limit(1, S, 200, 5).status, Status::DuplicateId);
  EXPECT_EQ(book.add_market(1, S, 5).status, Status::DuplicateId);
  EXPECT_EQ(book.order_count(), 1u);
  EXPECT_FALSE(book.best_ask());
}

TEST(Validation, IdCanBeReusedAfterOrderIsGone) {
  OrderBook book;
  book.add_limit(1, B, 100, 5);
  book.cancel(1);
  EXPECT_EQ(book.add_limit(1, S, 101, 5).status, Status::Ok);
}

// --- Execute (feed-reported fills) -----------------------------------------

TEST(Execute, PartialExecutionKeepsPriorityAndRecordsTrade) {
  OrderBook book;
  book.add_limit(1, S, 100, 10);
  book.add_limit(2, S, 100, 10);
  EXPECT_EQ(book.execute(1, 4), Status::Ok);
  const std::vector<Trade> expected = {{1, kExternalTaker, 100, 4, B}};
  EXPECT_EQ(book.last_trades(), expected);
  EXPECT_EQ(book.find(1)->qty, 6u);
  book.add_market(3, B, 1);
  EXPECT_EQ(book.last_trades()[0].maker_id, 1u);
}

TEST(Execute, FullExecutionRemovesOrder) {
  OrderBook book;
  book.add_limit(1, B, 100, 5);
  EXPECT_EQ(book.execute(1, 5), Status::Ok);
  EXPECT_FALSE(book.contains(1));
  EXPECT_FALSE(book.best_bid());
}

TEST(Execute, RejectsZeroOversizedAndUnknown) {
  OrderBook book;
  book.add_limit(1, B, 100, 5);
  EXPECT_EQ(book.execute(1, 0), Status::InvalidQty);
  EXPECT_EQ(book.execute(1, 6), Status::InvalidQty);
  EXPECT_EQ(book.execute(2, 1), Status::UnknownId);
  EXPECT_EQ(book.find(1)->qty, 5u);
}

// --- Pool reuse -----------------------------------------------------------

TEST(Pool, SlotsAreReusedWithoutCorruptingQueues) {
  OrderBook book(4);  // tiny reserve forces growth too
  for (int round = 0; round < 50; ++round) {
    const OrderId base = static_cast<OrderId>(round) * 10 + 1;
    for (OrderId k = 0; k < 8; ++k) book.add_limit(base + k, S, 100 + static_cast<Price>(k % 3), 2);
    for (OrderId k = 0; k < 8; k += 2) EXPECT_EQ(book.cancel(base + k), Status::Ok);
    book.add_market(1'000'000 + base, B, 5);
  }
  // Remaining resting orders must still be consistent with the depth view.
  std::uint64_t total = 0;
  std::uint32_t orders = 0;
  for (const DepthLevel& d : book.depth(S, 100)) {
    total += d.qty;
    orders += d.orders;
  }
  EXPECT_EQ(orders, book.order_count());
  EXPECT_EQ(total, 2u * orders);
}

}  // namespace
