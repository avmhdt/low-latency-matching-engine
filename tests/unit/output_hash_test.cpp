#include "llme/output_hash.hpp"

#include <gtest/gtest.h>

#include <vector>

namespace llme {
namespace {

OutMsg sample(std::uint64_t seq) {
  OutMsg m;
  m.type = to_wire(OutType::Fill);
  m.seq = seq;
  m.qty = 5;
  m.price = 100;
  return m;
}

TEST(OutputHashTest, StreamingMatchesOneShot) {
  std::vector<OutMsg> records;
  OutputHasher hasher;
  for (std::uint64_t i = 1; i <= 1000; ++i) {
    records.push_back(sample(i));
    hasher.update(records.back());
  }
  EXPECT_EQ(hasher.digest(), xxh3_bytes(std::as_bytes(std::span(records))));
}

TEST(OutputHashTest, EmptyStreamIsStable) {
  const OutputHasher hasher;
  EXPECT_EQ(hasher.digest(), xxh3_bytes({}));
}

TEST(OutputHashTest, DigestDoesNotConsumeState) {
  OutputHasher hasher;
  hasher.update(sample(1));
  const auto first = hasher.digest();
  EXPECT_EQ(hasher.digest(), first);
  hasher.update(sample(2));
  EXPECT_NE(hasher.digest(), first);
}

TEST(OutputHashTest, OrderMatters) {
  OutputHasher ab;
  ab.update(sample(1));
  ab.update(sample(2));
  OutputHasher ba;
  ba.update(sample(2));
  ba.update(sample(1));
  EXPECT_NE(ab.digest(), ba.digest());
}

TEST(OutputHashTest, FormatsAsFixedWidthHex) {
  EXPECT_EQ(format_hash(0xabcULL), "0x0000000000000abc");
  EXPECT_EQ(format_hash(0xffffffffffffffffULL), "0xffffffffffffffff");
}

}  // namespace
}  // namespace llme
