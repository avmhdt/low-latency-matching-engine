#include "llme/messages.hpp"

#include <gtest/gtest.h>

#include <array>
#include <bit>
#include <cstdint>

namespace llme {
namespace {

TEST(MessagesTest, DefaultRecordsAreAllZero) {
  const auto in = std::bit_cast<std::array<std::uint8_t, sizeof(InMsg)>>(InMsg{});
  const auto out = std::bit_cast<std::array<std::uint8_t, sizeof(OutMsg)>>(OutMsg{});
  for (const auto b : in) {
    EXPECT_EQ(b, 0);
  }
  for (const auto b : out) {
    EXPECT_EQ(b, 0);
  }
}

TEST(MessagesTest, EnumsUseSpecWireValues) {
  EXPECT_EQ(to_wire(MsgType::Modify), 3);
  EXPECT_EQ(to_wire(OutType::Reject), 2);
  EXPECT_EQ(to_wire(OutType::LevelUpdate), 9);
  EXPECT_EQ(to_wire(RejectReason::InvalidSide), 7);
  EXPECT_EQ(to_wire(CancelReason::SelfTrade), 5);
  EXPECT_EQ(opposite(Side::Buy), Side::Sell);
  EXPECT_EQ(opposite(Side::Sell), Side::Buy);
}

}  // namespace
}  // namespace llme
