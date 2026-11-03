#include <gtest/gtest.h>

#include <vector>

#include "lob/messages.hpp"

namespace {

using namespace lob;

std::vector<Message> samples() {
  return {make_add(1, Side::Buy, 10000, 50),
          make_add(0xFFFFFFFFFFFFFFFFull, Side::Sell, 2147483647, 4294967295u),
          make_market(7, Side::Sell, 12),
          make_cancel(42),
          make_modify(43, 9999, 7),
          make_execute(44, 3)};
}

TEST(Codec, RoundTripEachMessageType) {
  for (const Message& m : samples()) {
    std::vector<std::uint8_t> buf;
    encode(m, buf);
    EXPECT_EQ(buf.size(), encoded_size(m.type));
    Message out;
    const DecodeResult r = decode(buf, out);
    ASSERT_EQ(r.status, DecodeStatus::Ok);
    EXPECT_EQ(r.consumed, buf.size());
    EXPECT_EQ(out, m);
  }
}

TEST(Codec, WireSizesAreAsDocumented) {
  EXPECT_EQ(encoded_size(MsgType::Add), 19u);
  EXPECT_EQ(encoded_size(MsgType::Cancel), 9u);
  EXPECT_EQ(encoded_size(MsgType::Modify), 17u);
  EXPECT_EQ(encoded_size(MsgType::Execute), 13u);
  EXPECT_EQ(encoded_size(static_cast<MsgType>('?')), 0u);
}

TEST(Codec, KnownBytesLittleEndian) {
  std::vector<std::uint8_t> buf;
  encode(make_execute(0x0102030405060708ull, 0x0A0B0C0Du), buf);
  const std::vector<std::uint8_t> expected = {'E', 8, 7, 6, 5, 4, 3, 2, 1,
                                              0x0D, 0x0C, 0x0B, 0x0A};
  EXPECT_EQ(buf, expected);
}

TEST(Codec, TruncatedInputNeedsMoreAtEveryLength) {
  for (const Message& m : samples()) {
    std::vector<std::uint8_t> buf;
    encode(m, buf);
    for (std::size_t len = 0; len < buf.size(); ++len) {
      Message out;
      const DecodeResult r =
          decode(std::span<const std::uint8_t>(buf.data(), len), out);
      EXPECT_EQ(r.status, DecodeStatus::NeedMore);
      EXPECT_EQ(r.consumed, 0u);
    }
  }
}

TEST(Codec, UnknownTypeAndBadFieldsAreRejected) {
  Message out;
  const std::vector<std::uint8_t> bad_type = {'Z', 0, 0, 0, 0, 0, 0, 0, 0};
  EXPECT_EQ(decode(bad_type, out).status, DecodeStatus::BadType);

  std::vector<std::uint8_t> buf;
  encode(make_add(1, Side::Buy, 1, 1), buf);
  buf[1] = 2;  // side out of range
  EXPECT_EQ(decode(buf, out).status, DecodeStatus::BadField);
  buf[1] = 0;
  buf[2] = 9;  // order type out of range
  EXPECT_EQ(decode(buf, out).status, DecodeStatus::BadField);
}

TEST(Codec, DecodeWalksAConcatenatedStream) {
  const std::vector<Message> in = samples();
  const std::vector<std::uint8_t> file = encode_feed(in);
  ASSERT_TRUE(check_file_header(file));
  std::span<const std::uint8_t> rest(file.data() + kFileHeaderSize,
                                     file.size() - kFileHeaderSize);
  std::vector<Message> out;
  while (!rest.empty()) {
    Message m;
    const DecodeResult r = decode(rest, m);
    ASSERT_EQ(r.status, DecodeStatus::Ok);
    out.push_back(m);
    rest = rest.subspan(r.consumed);
  }
  EXPECT_EQ(out, in);
}

TEST(Codec, FileHeaderValidation) {
  std::vector<std::uint8_t> file = encode_feed({});
  EXPECT_EQ(file.size(), kFileHeaderSize);
  EXPECT_TRUE(check_file_header(file));
  file[0] = 'X';
  EXPECT_FALSE(check_file_header(file));
  file[0] = 'L';
  file[4] = 99;  // wrong version
  EXPECT_FALSE(check_file_header(file));
  EXPECT_FALSE(check_file_header(std::span<const std::uint8_t>(file.data(), 3)));
}

}  // namespace
