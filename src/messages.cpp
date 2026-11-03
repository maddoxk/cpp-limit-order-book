#include "lob/messages.hpp"

#include <iterator>

namespace lob {
namespace {

void put_u32(std::vector<std::uint8_t>& out, std::uint32_t v) {
  for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
}

void put_u64(std::vector<std::uint8_t>& out, std::uint64_t v) {
  for (int i = 0; i < 8; ++i) out.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
}

std::uint32_t get_u32(const std::uint8_t* p) {
  std::uint32_t v = 0;
  for (int i = 3; i >= 0; --i) v = (v << 8) | p[i];
  return v;
}

std::uint64_t get_u64(const std::uint8_t* p) {
  std::uint64_t v = 0;
  for (int i = 7; i >= 0; --i) v = (v << 8) | p[i];
  return v;
}

constexpr std::uint8_t kMagic[4] = {'L', 'O', 'B', 'F'};

}  // namespace

Message make_add(OrderId id, Side side, Price price, Qty qty) {
  Message m;
  m.type = MsgType::Add;
  m.side = side;
  m.order_type = OrderType::Limit;
  m.id = id;
  m.price = price;
  m.qty = qty;
  return m;
}

Message make_market(OrderId id, Side side, Qty qty) {
  Message m;
  m.type = MsgType::Add;
  m.side = side;
  m.order_type = OrderType::Market;
  m.id = id;
  m.qty = qty;  // price stays 0: market orders carry no price
  return m;
}

Message make_cancel(OrderId id) {
  Message m;
  m.type = MsgType::Cancel;
  m.id = id;
  return m;
}

Message make_modify(OrderId id, Price price, Qty qty) {
  Message m;
  m.type = MsgType::Modify;
  m.id = id;
  m.price = price;
  m.qty = qty;
  return m;
}

Message make_execute(OrderId id, Qty qty) {
  Message m;
  m.type = MsgType::Execute;
  m.id = id;
  m.qty = qty;
  return m;
}

std::size_t encoded_size(MsgType t) noexcept {
  switch (t) {
    case MsgType::Add: return 19;
    case MsgType::Cancel: return 9;
    case MsgType::Modify: return 17;
    case MsgType::Execute: return 13;
  }
  return 0;  // unknown byte value
}

void encode(const Message& m, std::vector<std::uint8_t>& out) {
  out.push_back(static_cast<std::uint8_t>(m.type));
  switch (m.type) {
    case MsgType::Add:
      out.push_back(static_cast<std::uint8_t>(m.side));
      out.push_back(static_cast<std::uint8_t>(m.order_type));
      put_u64(out, m.id);
      put_u32(out, static_cast<std::uint32_t>(m.price));
      put_u32(out, m.qty);
      break;
    case MsgType::Cancel:
      put_u64(out, m.id);
      break;
    case MsgType::Modify:
      put_u64(out, m.id);
      put_u32(out, static_cast<std::uint32_t>(m.price));
      put_u32(out, m.qty);
      break;
    case MsgType::Execute:
      put_u64(out, m.id);
      put_u32(out, m.qty);
      break;
  }
}

void write_file_header(std::vector<std::uint8_t>& out) {
  out.insert(out.end(), std::begin(kMagic), std::end(kMagic));
  put_u32(out, kFileVersion);
}

std::vector<std::uint8_t> encode_feed(std::span<const Message> messages) {
  std::vector<std::uint8_t> out;
  out.reserve(kFileHeaderSize + messages.size() * 16);
  write_file_header(out);
  for (const Message& m : messages) encode(m, out);
  return out;
}

const char* to_string(DecodeStatus s) noexcept {
  switch (s) {
    case DecodeStatus::Ok: return "Ok";
    case DecodeStatus::NeedMore: return "NeedMore (truncated message)";
    case DecodeStatus::BadType: return "BadType (unknown message type)";
    case DecodeStatus::BadField: return "BadField (side/order_type out of range)";
  }
  return "?";
}

DecodeResult decode(std::span<const std::uint8_t> in, Message& out) {
  if (in.empty()) return {DecodeStatus::NeedMore, 0};
  const auto type = static_cast<MsgType>(in[0]);
  const std::size_t size = encoded_size(type);
  if (size == 0) return {DecodeStatus::BadType, 0};
  if (in.size() < size) return {DecodeStatus::NeedMore, 0};

  const std::uint8_t* p = in.data();
  Message m;
  m.type = type;
  switch (type) {
    case MsgType::Add:
      if (p[1] > 1 || p[2] > 1) return {DecodeStatus::BadField, 0};
      m.side = static_cast<Side>(p[1]);
      m.order_type = static_cast<OrderType>(p[2]);
      m.id = get_u64(p + 3);
      m.price = static_cast<Price>(get_u32(p + 11));
      m.qty = get_u32(p + 15);
      break;
    case MsgType::Cancel:
      m.id = get_u64(p + 1);
      break;
    case MsgType::Modify:
      m.id = get_u64(p + 1);
      m.price = static_cast<Price>(get_u32(p + 9));
      m.qty = get_u32(p + 13);
      break;
    case MsgType::Execute:
      m.id = get_u64(p + 1);
      m.qty = get_u32(p + 9);
      break;
  }
  out = m;
  return {DecodeStatus::Ok, size};
}

bool check_file_header(std::span<const std::uint8_t> in) noexcept {
  if (in.size() < kFileHeaderSize) return false;
  for (int i = 0; i < 4; ++i) {
    if (in[static_cast<std::size_t>(i)] != kMagic[i]) return false;
  }
  return get_u32(in.data() + 4) == kFileVersion;
}

}  // namespace lob
