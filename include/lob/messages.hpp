// A small ITCH-style binary feed format.
//
// Like Nasdaq ITCH, every message starts with a one-byte type and has a fixed
// size per type, so a decoder can walk a byte stream without length prefixes.
// All integers are little-endian and are read/written byte by byte (no struct
// casts), so the code has no alignment or endianness assumptions.
//
//   Add     'A'  type(1) side(1) order_type(1) id(8) price(4) qty(4)  = 19 bytes
//   Cancel  'X'  type(1) id(8)                                        =  9 bytes
//   Modify  'M'  type(1) id(8) price(4) qty(4)                        = 17 bytes
//   Execute 'E'  type(1) id(8) qty(4)                                 = 13 bytes
//
// A feed *file* is an 8-byte header ("LOBF" + u32 version) followed by
// back-to-back messages.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "lob/types.hpp"

namespace lob {

enum class MsgType : std::uint8_t {
  Add = 'A',
  Cancel = 'X',
  Modify = 'M',
  Execute = 'E',
};

enum class OrderType : std::uint8_t { Limit = 0, Market = 1 };

// In-memory form of any message. Fields that a message type does not carry are
// left at their defaults. Trivially copyable so it can travel through the
// lock-free queue by value.
struct Message {
  MsgType type = MsgType::Add;
  Side side = Side::Buy;
  OrderType order_type = OrderType::Limit;
  OrderId id = 0;
  Price price = 0;
  Qty qty = 0;

  bool operator==(const Message&) const = default;
};

Message make_add(OrderId id, Side side, Price price, Qty qty);
Message make_market(OrderId id, Side side, Qty qty);
Message make_cancel(OrderId id);
Message make_modify(OrderId id, Price price, Qty qty);
Message make_execute(OrderId id, Qty qty);

inline constexpr std::size_t kMaxMessageSize = 19;
inline constexpr std::size_t kFileHeaderSize = 8;
inline constexpr std::uint32_t kFileVersion = 1;

// Wire size of a message type, or 0 if `t` is not a known type.
std::size_t encoded_size(MsgType t) noexcept;

// Append the wire form of `m` to `out`.
void encode(const Message& m, std::vector<std::uint8_t>& out);

// Append the 8-byte file header.
void write_file_header(std::vector<std::uint8_t>& out);

// Header + all messages.
std::vector<std::uint8_t> encode_feed(std::span<const Message> messages);

enum class DecodeStatus : std::uint8_t {
  Ok,
  NeedMore,  // input ends in the middle of a message (truncated)
  BadType,   // unknown type byte
  BadField,  // side / order_type byte out of range
};

const char* to_string(DecodeStatus s) noexcept;

struct DecodeResult {
  DecodeStatus status;
  std::size_t consumed;  // bytes used; 0 unless status == Ok
};

// Decode ONE message from the front of `in`. Never reads past `in`.
DecodeResult decode(std::span<const std::uint8_t> in, Message& out);

// True if `in` starts with a valid file header.
bool check_file_header(std::span<const std::uint8_t> in) noexcept;

}  // namespace lob
