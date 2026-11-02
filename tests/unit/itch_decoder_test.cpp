#include "itch_decoder.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

namespace llme::workload {
namespace {

using Bytes = std::vector<std::uint8_t>;

constexpr std::int64_t kInSession = 40000000000000;  // 11:06:40 ET

void put(Bytes& out, std::uint64_t value, std::size_t width) {
  for (std::size_t i = width; i > 0; --i) {
    out.push_back(static_cast<std::uint8_t>(value >> (8 * (i - 1))));
  }
}

Bytes header(char type, std::uint16_t locate, std::int64_t ts = kInSession) {
  Bytes m;
  m.push_back(static_cast<std::uint8_t>(type));
  put(m, locate, 2);
  put(m, 0, 2);
  put(m, static_cast<std::uint64_t>(ts), 6);
  return m;
}

void put_stock(Bytes& m, const std::string& name) {
  for (std::size_t i = 0; i < 8; ++i) {
    m.push_back(i < name.size() ? static_cast<std::uint8_t>(name[i]) : ' ');
  }
}

Bytes add(std::uint16_t locate, OrderId ref, char side, std::uint32_t shares, std::uint32_t price,
          char type = 'A', std::int64_t ts = kInSession) {
  Bytes m = header(type, locate, ts);
  put(m, ref, 8);
  m.push_back(static_cast<std::uint8_t>(side));
  put(m, shares, 4);
  put_stock(m, "AAPL");
  put(m, price, 4);
  if (type == 'F') {
    put_stock(m, "MPID");
    m.resize(40);
  }
  return m;
}

Bytes exec(std::uint16_t locate, OrderId ref, std::uint32_t shares) {
  Bytes m = header('E', locate);
  put(m, ref, 8);
  put(m, shares, 4);
  put(m, 99, 8);
  return m;
}

Bytes exec_price(std::uint16_t locate, OrderId ref, std::uint32_t shares, char printable) {
  Bytes m = header('C', locate);
  put(m, ref, 8);
  put(m, shares, 4);
  put(m, 99, 8);
  m.push_back(static_cast<std::uint8_t>(printable));
  put(m, 1500000, 4);
  return m;
}

Bytes cancel(std::uint16_t locate, OrderId ref, std::uint32_t shares) {
  Bytes m = header('X', locate);
  put(m, ref, 8);
  put(m, shares, 4);
  return m;
}

Bytes erase(std::uint16_t locate, OrderId ref) {
  Bytes m = header('D', locate);
  put(m, ref, 8);
  return m;
}

Bytes replace(std::uint16_t locate, OrderId ref, OrderId new_ref, std::uint32_t shares,
              std::uint32_t price) {
  Bytes m = header('U', locate);
  put(m, ref, 8);
  put(m, new_ref, 8);
  put(m, shares, 4);
  put(m, price, 4);
  return m;
}

Bytes directory(std::uint16_t locate, const std::string& name) {
  Bytes m = header('R', locate);
  put_stock(m, name);
  m.resize(39);
  return m;
}

Bytes padded(char type, std::uint16_t locate, std::size_t size) {
  Bytes m = header(type, locate);
  m.resize(size);
  return m;
}

ItchEvent parse_ok(const Bytes& m) {
  ItchEvent event;
  bool malformed = true;
  EXPECT_TRUE(parse_itch(m, event, malformed));
  EXPECT_FALSE(malformed);
  return event;
}

Bytes framed(const std::vector<Bytes>& messages) {
  Bytes out;
  for (const Bytes& m : messages) {
    put(out, m.size(), 2);
    out.insert(out.end(), m.begin(), m.end());
  }
  return out;
}

TEST(ItchParseTest, ReadsAddOrder) {
  const auto event = parse_ok(add(9, 1234, 'S', 100, 1500000));
  EXPECT_EQ(event.type, 'A');
  EXPECT_EQ(event.locate, 9);
  EXPECT_EQ(event.timestamp_ns, kInSession);
  EXPECT_EQ(event.ref, 1234U);
  EXPECT_EQ(event.side, Side::Sell);
  EXPECT_EQ(event.shares, 100U);
  EXPECT_EQ(event.price, 1500000U);
  EXPECT_EQ(stock_name(event.stock), "AAPL");
}

TEST(ItchParseTest, ReadsAddWithMpid) {
  const auto event = parse_ok(add(9, 1234, 'B', 100, 1500000, 'F'));
  EXPECT_EQ(event.type, 'F');
  EXPECT_EQ(event.side, Side::Buy);
}

TEST(ItchParseTest, ReadsExecutions) {
  EXPECT_EQ(parse_ok(exec(1, 5, 7)).shares, 7U);
  EXPECT_TRUE(parse_ok(exec(1, 5, 7)).printable);
  EXPECT_TRUE(parse_ok(exec_price(1, 5, 7, 'Y')).printable);
  EXPECT_FALSE(parse_ok(exec_price(1, 5, 7, 'N')).printable);
}

TEST(ItchParseTest, ReadsCancelDeleteReplaceDirectory) {
  EXPECT_EQ(parse_ok(cancel(1, 5, 7)).shares, 7U);
  EXPECT_EQ(parse_ok(erase(1, 5)).ref, 5U);
  const auto u = parse_ok(replace(1, 5, 6, 8, 1510000));
  EXPECT_EQ(u.new_ref, 6U);
  EXPECT_EQ(u.shares, 8U);
  EXPECT_EQ(u.price, 1510000U);
  EXPECT_EQ(stock_name(parse_ok(directory(3, "MSFT")).stock), "MSFT");
}

TEST(ItchParseTest, FlagsWrongLengthsAndBadSides) {
  ItchEvent event;
  bool malformed = false;
  for (const char type : {'A', 'F', 'E', 'C', 'X', 'D', 'U', 'R', 'P', 'Q', 'H'}) {
    EXPECT_FALSE(parse_itch(padded(type, 1, 12), event, malformed)) << type;
    EXPECT_TRUE(malformed) << type;
  }
  EXPECT_FALSE(parse_itch(add(1, 2, 'Z', 1, 1), event, malformed));
  EXPECT_TRUE(malformed);
  EXPECT_FALSE(parse_itch({}, event, malformed));
  EXPECT_TRUE(malformed);
}

TEST(ItchParseTest, IgnoresOtherTypesQuietly) {
  ItchEvent event;
  bool malformed = true;
  const Bytes system_event = padded('S', 0, 12);
  EXPECT_FALSE(parse_itch(system_event, event, malformed));
  EXPECT_FALSE(malformed);
}

TEST(ItchFrameTest, SplitsFramesAcrossRefills) {
  const auto bytes = framed({add(1, 1, 'B', 1, 100), erase(1, 1), cancel(1, 2, 3)});
  MemorySource source(bytes);
  ItchFrameReader reader(source);
  std::span<const std::uint8_t> message;
  std::vector<std::size_t> sizes;
  while (reader.next(message)) {
    sizes.push_back(message.size());
  }
  EXPECT_EQ(sizes, (std::vector<std::size_t>{36, 19, 23}));
  EXPECT_FALSE(reader.truncated());
}

TEST(ItchFrameTest, ReportsAFrameCutShort) {
  auto bytes = framed({add(1, 1, 'B', 1, 100)});
  bytes.resize(bytes.size() - 3);
  MemorySource source(bytes);
  ItchFrameReader reader(source);
  std::span<const std::uint8_t> message;
  EXPECT_FALSE(reader.next(message));
  EXPECT_TRUE(reader.truncated());
}

TEST(ItchFrameTest, ReportsALengthCutShort) {
  const Bytes bytes = {0};
  MemorySource source(bytes);
  ItchFrameReader reader(source);
  std::span<const std::uint8_t> message;
  EXPECT_FALSE(reader.next(message));
  EXPECT_TRUE(reader.truncated());
}

TEST(ItchFrameTest, CarriesBytesOverTheBufferEdge) {
  // Far more than the 1 MiB buffer, so frames straddle a refill.
  std::vector<Bytes> messages(100000, add(1, 1, 'B', 1, 100));
  const auto bytes = framed(messages);
  MemorySource source(bytes);
  ItchFrameReader reader(source);
  std::span<const std::uint8_t> message;
  std::size_t count = 0;
  while (reader.next(message)) {
    ASSERT_EQ(message.size(), 36U);
    ASSERT_EQ(message[0], 'A');
    ++count;
  }
  EXPECT_EQ(count, messages.size());
}

TEST(FileSourceTest, ReadsPlainFilesAndRefusesMissingOnes) {
  const auto path = std::filesystem::temp_directory_path() / "llme_itch_source.bin";
  {
    std::FILE* file = std::fopen(path.c_str(), "wb");
    ASSERT_NE(file, nullptr);
    ASSERT_EQ(std::fwrite("abcd", 1, 4, file), 4U);
    ASSERT_EQ(std::fclose(file), 0);
  }
  FileSource source(path);
  std::array<std::uint8_t, 8> out{};
  EXPECT_EQ(source.read(out.data(), out.size()), 4U);
  EXPECT_EQ(out[3], 'd');
  EXPECT_EQ(source.read(out.data(), out.size()), 0U);
  std::filesystem::remove(path);
  EXPECT_THROW(FileSource("/nonexistent-directory/x"), std::runtime_error);
}

class ItchConvertTest : public testing::Test {
 protected:
  void SetUp() override { converter_.select(1, &builder_); }

  void feed(const Bytes& m) { converter_.on_event(parse_ok(m)); }

  std::vector<InMsg> msgs_;
  ReplayBuilder builder_{[this](const InMsg& m) { msgs_.push_back(m); }};
  ItchConverter converter_;
};

TEST_F(ItchConvertTest, MapsEveryOrderMessage) {
  feed(add(1, 1, 'B', 100, 1500000));
  feed(add(1, 2, 'S', 100, 1510000, 'F'));
  feed(exec(1, 1, 10));
  feed(exec_price(1, 1, 10, 'Y'));
  feed(cancel(1, 1, 10));
  feed(replace(1, 1, 3, 50, 1490000));
  feed(erase(1, 2));
  const auto& s = builder_.stats();
  EXPECT_EQ(s.adds, 2U);
  EXPECT_EQ(s.out_ioc, 2U);
  EXPECT_EQ(s.out_modify, 1U);
  EXPECT_EQ(s.replaces, 1U);
  EXPECT_EQ(s.out_cancel, 2U);
  EXPECT_EQ(s.out_new, 3U);
}

TEST_F(ItchConvertTest, NonPrintableExecutionReducesWithoutMatching) {
  feed(add(1, 1, 'B', 100, 1500000));
  feed(exec_price(1, 1, 10, 'N'));
  EXPECT_EQ(msgs_.back().type, to_wire(MsgType::Modify));
  EXPECT_EQ(msgs_.back().qty, 90U);
  EXPECT_EQ(builder_.stats().skipped_cross, 1U);
}

TEST_F(ItchConvertTest, IgnoresUnselectedSymbols) {
  feed(add(2, 1, 'B', 100, 1500000));
  EXPECT_TRUE(msgs_.empty());
  EXPECT_EQ(builder_.stats().events, 0U);
}

TEST_F(ItchConvertTest, AppliesTheSessionWindow) {
  feed(add(1, 1, 'B', 100, 1500000, 'A', kSessionStartNs - 1));
  feed(add(1, 2, 'B', 100, 1500000, 'A', kSessionEndNs));
  EXPECT_TRUE(msgs_.empty());
  EXPECT_EQ(builder_.stats().skipped_session, 2U);
  feed(add(1, 3, 'B', 100, 1500000, 'A', kSessionStartNs));
  EXPECT_EQ(msgs_.size(), 1U);
}

TEST_F(ItchConvertTest, CountsWhatItSkips) {
  feed(padded('P', 1, 44));
  feed(padded('Q', 1, 40));
  feed(padded('H', 1, 25));
  feed(directory(1, "AAPL"));
  const auto& s = builder_.stats();
  EXPECT_EQ(s.skipped_hidden, 1U);
  EXPECT_EQ(s.skipped_cross, 1U);
  EXPECT_EQ(s.skipped_halt, 1U);
  EXPECT_TRUE(msgs_.empty());
}

TEST(ItchNamesTest, TrimsPadding) {
  EXPECT_EQ(stock_name({'A', ' ', ' ', ' ', ' ', ' ', ' ', ' '}), "A");
  EXPECT_EQ(stock_name({'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H'}), "ABCDEFGH");
  EXPECT_TRUE(is_order_message('U'));
  EXPECT_FALSE(is_order_message('R'));
}

}  // namespace
}  // namespace llme::workload
