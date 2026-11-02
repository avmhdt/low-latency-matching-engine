#include "llme/mlog.hpp"

#include <gtest/gtest.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace llme {
namespace {

std::filesystem::path temp_path(const std::string& name) {
  return std::filesystem::temp_directory_path() / ("llme_mlog_test_" + name);
}

InMsg sample_msg(std::uint64_t i) {
  InMsg m;
  m.type = to_wire(MsgType::New);
  m.side = to_wire(i % 2 == 0 ? Side::Buy : Side::Sell);
  m.qty = static_cast<Qty>(i + 1);
  m.order_id = i + 1;
  m.price = 100 + static_cast<Price>(i);
  return m;
}

// A valid in-memory .mlog image with the given records, aligned for the reader.
std::vector<std::uint64_t> image_with_records(std::size_t n) {
  std::vector<std::uint64_t> words((sizeof(MlogHeader) + n * sizeof(InMsg)) / 8);
  MlogHeader header;
  header.magic = kMlogMagic;
  header.version = kLogVersion;
  header.record_size = sizeof(InMsg);
  header.record_count = n;
  std::memcpy(words.data(), &header, sizeof(header));
  return words;
}

MlogHeader* header_of(std::vector<std::uint64_t>& image) {
  return reinterpret_cast<MlogHeader*>(image.data());
}

MlogError error_of(const std::vector<std::uint64_t>& image) {
  const auto reader = MlogReader::from_bytes(std::as_bytes(std::span(image)));
  EXPECT_FALSE(reader.has_value());
  return reader.error();
}

TEST(MlogTest, RoundTripsTenThousandMessages) {
  const auto path = temp_path("round_trip.mlog");
  auto writer = MlogWriter::create(path, {.symbol_id = 7, .seed = 42});
  ASSERT_TRUE(writer.has_value());
  constexpr std::uint64_t kCount = 10000;
  for (std::uint64_t i = 0; i < kCount; ++i) {
    ASSERT_TRUE(writer->write(sample_msg(i)));
  }
  ASSERT_TRUE(writer->close().has_value());

  const auto reader = MlogReader::open(path);
  ASSERT_TRUE(reader.has_value()) << to_string(reader.error());
  EXPECT_EQ(reader->header().symbol_id, 7U);
  EXPECT_EQ(reader->header().seed, 42U);
  EXPECT_EQ(reader->header().record_size, sizeof(InMsg));
  ASSERT_EQ(reader->records().size(), kCount);
  for (std::uint64_t i = 0; i < kCount; ++i) {
    const auto expected_msg = sample_msg(i);
    ASSERT_EQ(std::memcmp(&reader->records()[i], &expected_msg, sizeof(InMsg)), 0);
  }
  std::filesystem::remove(path);
}

TEST(MlogTest, RoundTripsOutputLog) {
  const auto path = temp_path("round_trip.olog");
  auto writer = OlogWriter::create(path);
  ASSERT_TRUE(writer.has_value());
  OutMsg m;
  m.type = to_wire(OutType::Ack);
  m.seq = 1;
  ASSERT_TRUE(writer->write(m));
  ASSERT_TRUE(writer->close().has_value());
  const auto reader = OlogReader::open(path);
  ASSERT_TRUE(reader.has_value());
  ASSERT_EQ(reader->records().size(), 1U);
  EXPECT_EQ(reader->records()[0], m);
  EXPECT_FALSE(MlogReader::open(path).has_value());
  std::filesystem::remove(path);
}

TEST(MlogTest, EmptyLogIsValid) {
  const auto image = image_with_records(0);
  const auto reader = MlogReader::from_bytes(std::as_bytes(std::span(image)));
  ASSERT_TRUE(reader.has_value());
  EXPECT_TRUE(reader->records().empty());
}

TEST(MlogTest, RejectsMissingFile) {
  const auto reader = MlogReader::open(temp_path("does_not_exist.mlog"));
  ASSERT_FALSE(reader.has_value());
  EXPECT_EQ(reader.error(), MlogError::OpenFailed);
}

TEST(MlogTest, RejectsDirectory) {
  const auto reader = MlogReader::open(std::filesystem::temp_directory_path());
  ASSERT_FALSE(reader.has_value());
  EXPECT_EQ(reader.error(), MlogError::OpenFailed);
}

TEST(MlogTest, RejectsShortFile) {
  const auto path = temp_path("short.mlog");
  std::ofstream(path) << "short";
  const auto reader = MlogReader::open(path);
  ASSERT_FALSE(reader.has_value());
  EXPECT_EQ(reader.error(), MlogError::TooSmall);
  std::filesystem::remove(path);
}

TEST(MlogTest, RejectsWriterThatCannotOpen) {
  const auto writer = MlogWriter::create("/nonexistent-directory/x.mlog");
  ASSERT_FALSE(writer.has_value());
  EXPECT_EQ(writer.error(), MlogError::OpenFailed);
}

TEST(MlogTest, RejectsBadMagic) {
  auto image = image_with_records(1);
  header_of(image)->magic = kOlogMagic;
  EXPECT_EQ(error_of(image), MlogError::BadMagic);
}

TEST(MlogTest, RejectsBadVersion) {
  auto image = image_with_records(1);
  header_of(image)->version = 2;
  EXPECT_EQ(error_of(image), MlogError::BadVersion);
}

TEST(MlogTest, RejectsBadRecordSize) {
  auto image = image_with_records(1);
  header_of(image)->record_size = 48;
  EXPECT_EQ(error_of(image), MlogError::BadRecordSize);
}

TEST(MlogTest, RejectsRecordCountMismatch) {
  auto image = image_with_records(2);
  header_of(image)->record_count = 3;
  EXPECT_EQ(error_of(image), MlogError::BadRecordCount);
}

TEST(MlogTest, RejectsTruncatedRecord) {
  const auto image = image_with_records(2);
  const auto bytes = std::as_bytes(std::span(image)).first(sizeof(MlogHeader) + sizeof(InMsg) + 8);
  const auto reader = MlogReader::from_bytes(bytes);
  ASSERT_FALSE(reader.has_value());
  EXPECT_EQ(reader.error(), MlogError::BadRecordCount);
}

TEST(MlogTest, RejectsNonZeroReservedBytes) {
  auto image = image_with_records(1);
  header_of(image)->reserved[22] = 1;
  EXPECT_EQ(error_of(image), MlogError::ReservedNonZero);
}

TEST(MlogTest, RejectsMisalignedImage) {
  const auto image = image_with_records(1);
  const auto bytes = std::as_bytes(std::span(image)).subspan(1);
  const auto reader = MlogReader::from_bytes(bytes);
  ASSERT_FALSE(reader.has_value());
  EXPECT_EQ(reader.error(), MlogError::Misaligned);
}

TEST(MlogTest, ErrorsHaveMessages) {
  for (const auto e :
       {MlogError::OpenFailed, MlogError::TooSmall, MlogError::BadMagic, MlogError::BadVersion,
        MlogError::BadRecordSize, MlogError::BadRecordCount, MlogError::ReservedNonZero,
        MlogError::Misaligned, MlogError::MapFailed, MlogError::WriteFailed}) {
    EXPECT_FALSE(to_string(e).empty());
  }
}

}  // namespace
}  // namespace llme
