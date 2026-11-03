// Basic value types shared by the whole project.
//
// Prices are integers measured in "ticks" (the smallest price increment).
// A tick-based integer price avoids floating-point rounding, makes price
// equality exact, and lets us use prices as array / map keys.
#pragma once

#include <cstdint>

namespace lob {

using OrderId = std::uint64_t;  // 0 is reserved ("no order")
using Price = std::int32_t;     // ticks; valid prices are > 0
using Qty = std::uint32_t;      // shares / contracts

enum class Side : std::uint8_t { Buy = 0, Sell = 1 };

constexpr Side opposite(Side s) noexcept {
  return s == Side::Buy ? Side::Sell : Side::Buy;
}

// Result code for every book operation. Rejections never change the book.
enum class Status : std::uint8_t {
  Ok = 0,
  InvalidId,     // order id 0
  InvalidPrice,  // price <= 0
  InvalidQty,    // qty == 0 (or an execution larger than the resting qty)
  DuplicateId,   // id already rests in the book
  UnknownId,     // id is not in the book (never existed, filled, or cancelled)
};

constexpr const char* to_string(Status s) noexcept {
  switch (s) {
    case Status::Ok: return "Ok";
    case Status::InvalidId: return "InvalidId";
    case Status::InvalidPrice: return "InvalidPrice";
    case Status::InvalidQty: return "InvalidQty";
    case Status::DuplicateId: return "DuplicateId";
    case Status::UnknownId: return "UnknownId";
  }
  return "?";
}

// One fill. The trade always prints at the *resting* (maker) order's price.
struct Trade {
  OrderId maker_id;  // the resting order that was hit
  OrderId taker_id;  // the incoming order (kExternalTaker for feed executions)
  Price price;
  Qty qty;
  Side taker_side;

  bool operator==(const Trade&) const = default;
};

// taker_id used when an execution is reported by the feed rather than being
// produced by matching an incoming order.
inline constexpr OrderId kExternalTaker = 0;

}  // namespace lob
