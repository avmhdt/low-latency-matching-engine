#include "itch_decoder.hpp"

#include <zlib.h>

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace llme::workload {

namespace {

constexpr std::size_t kLengthBytes = 2;
constexpr std::size_t kMaxFrame = 2 + 65535;

// ITCH 5.0 message lengths, excluding the 2-byte length prefix.
constexpr std::size_t kLenAdd = 36;
constexpr std::size_t kLenAddMpid = 40;
constexpr std::size_t kLenExec = 31;
constexpr std::size_t kLenExecPrice = 36;
constexpr std::size_t kLenCancel = 23;
constexpr std::size_t kLenDelete = 19;
constexpr std::size_t kLenReplace = 35;
constexpr std::size_t kLenDirectory = 39;
constexpr std::size_t kLenTrade = 44;
constexpr std::size_t kLenCross = 40;
constexpr std::size_t kLenHalt = 25;

template <class T>
T big_endian(const std::uint8_t* bytes, std::size_t width) noexcept {
  std::uint64_t value = 0;
  for (std::size_t i = 0; i < width; ++i) {
    value = (value << 8U) | bytes[i];
  }
  return static_cast<T>(value);
}

}  // namespace

bool parse_itch(std::span<const std::uint8_t> message, ItchEvent& out, bool& malformed) noexcept {
  malformed = false;
  if (message.empty()) {
    malformed = true;
    return false;
  }
  const std::uint8_t* m = message.data();
  const auto size = message.size();
  const auto type = static_cast<char>(m[0]);

  const auto header = [&](std::size_t expected) noexcept {
    if (size != expected) {
      malformed = true;
      return false;
    }
    out.type = type;
    out.locate = big_endian<std::uint16_t>(m + 1, 2);
    out.timestamp_ns = big_endian<std::int64_t>(m + 5, 6);
    return true;
  };

  switch (type) {
    case 'A':
    case 'F': {
      if (!header(type == 'A' ? kLenAdd : kLenAddMpid)) {
        return false;
      }
      const std::uint8_t side = m[19];
      if (side != 'B' && side != 'S') {
        malformed = true;
        return false;
      }
      out.ref = big_endian<OrderId>(m + 11, 8);
      out.side = side == 'B' ? Side::Buy : Side::Sell;
      out.shares = big_endian<std::uint32_t>(m + 20, 4);
      std::memcpy(out.stock.data(), m + 24, out.stock.size());
      out.price = big_endian<std::uint32_t>(m + 32, 4);
      return true;
    }
    case 'E':
      if (!header(kLenExec)) {
        return false;
      }
      out.ref = big_endian<OrderId>(m + 11, 8);
      out.shares = big_endian<std::uint32_t>(m + 19, 4);
      out.printable = true;
      return true;
    case 'C':
      if (!header(kLenExecPrice)) {
        return false;
      }
      out.ref = big_endian<OrderId>(m + 11, 8);
      out.shares = big_endian<std::uint32_t>(m + 19, 4);
      out.printable = m[31] == 'Y';
      return true;
    case 'X':
      if (!header(kLenCancel)) {
        return false;
      }
      out.ref = big_endian<OrderId>(m + 11, 8);
      out.shares = big_endian<std::uint32_t>(m + 19, 4);
      return true;
    case 'D':
      if (!header(kLenDelete)) {
        return false;
      }
      out.ref = big_endian<OrderId>(m + 11, 8);
      return true;
    case 'U':
      if (!header(kLenReplace)) {
        return false;
      }
      out.ref = big_endian<OrderId>(m + 11, 8);
      out.new_ref = big_endian<OrderId>(m + 19, 8);
      out.shares = big_endian<std::uint32_t>(m + 27, 4);
      out.price = big_endian<std::uint32_t>(m + 31, 4);
      return true;
    case 'R':
      if (!header(kLenDirectory)) {
        return false;
      }
      std::memcpy(out.stock.data(), m + 11, out.stock.size());
      return true;
    case 'P':
      return header(kLenTrade);
    case 'Q':
      return header(kLenCross);
    case 'H':
      return header(kLenHalt);
    default:
      return false;
  }
}

std::size_t MemorySource::read(std::uint8_t* out, std::size_t size) {
  const std::size_t count = std::min(size, bytes_.size());
  if (count != 0) {
    std::memcpy(out, bytes_.data(), count);
    bytes_ = bytes_.subspan(count);
  }
  return count;
}

FileSource::FileSource(const std::filesystem::path& path) : file_(gzopen(path.c_str(), "rb")) {
  if (file_ == nullptr) {
    throw std::runtime_error("cannot open " + path.string());
  }
  gzbuffer(static_cast<gzFile>(file_), 1U << 20U);
}

FileSource::~FileSource() { gzclose(static_cast<gzFile>(file_)); }

std::size_t FileSource::read(std::uint8_t* out, std::size_t size) {
  const int count = gzread(static_cast<gzFile>(file_), out, static_cast<unsigned>(size));
  if (count < 0) {
    throw std::runtime_error("cannot read ITCH input");
  }
  return static_cast<std::size_t>(count);
}

ItchFrameReader::ItchFrameReader(ByteSource& source, std::size_t buffer_bytes)
    : source_(&source), buffer_(std::max(buffer_bytes, kMaxFrame)) {}

bool ItchFrameReader::ensure(std::size_t bytes) {
  if (end_ - start_ >= bytes) {
    return true;
  }
  std::memmove(buffer_.data(), buffer_.data() + start_, end_ - start_);
  end_ -= start_;
  start_ = 0;
  while (end_ < bytes) {
    const std::size_t got = source_->read(buffer_.data() + end_, buffer_.size() - end_);
    if (got == 0) {
      return false;
    }
    end_ += got;
  }
  return true;
}

bool ItchFrameReader::next(std::span<const std::uint8_t>& message) {
  if (!ensure(kLengthBytes)) {
    truncated_ = end_ != start_;
    return false;
  }
  const auto length = big_endian<std::size_t>(buffer_.data() + start_, kLengthBytes);
  if (!ensure(kLengthBytes + length)) {
    truncated_ = true;
    return false;
  }
  message = {buffer_.data() + start_ + kLengthBytes, length};
  start_ += kLengthBytes + length;
  return true;
}

ItchConverter::ItchConverter() : builders_(std::size_t{1} << 16U, nullptr) {}

void ItchConverter::select(std::uint16_t locate, ReplayBuilder* builder) noexcept {
  builders_[locate] = builder;
}

void ItchConverter::on_event(const ItchEvent& event) {
  ReplayBuilder* builder = builders_[event.locate];
  if (builder == nullptr) {
    return;
  }
  auto& stats = builder->stats();
  if (event.type == 'P') {
    ++stats.skipped_hidden;
    return;
  }
  if (event.type == 'Q') {
    ++stats.skipped_cross;
    return;
  }
  if (event.type == 'H') {
    ++stats.skipped_halt;
    return;
  }
  if (!is_order_message(event.type)) {
    return;
  }
  if (event.timestamp_ns < kSessionStartNs || event.timestamp_ns >= kSessionEndNs) {
    ++stats.skipped_session;
    return;
  }
  switch (event.type) {
    case 'A':
    case 'F':
      builder->add(event.ref, event.side, event.price, event.shares);
      break;
    case 'E':
      builder->exec(event.ref, event.shares);
      break;
    case 'C':
      if (event.printable) {
        builder->exec(event.ref, event.shares);
      } else {
        builder->cross_exec(event.ref, event.shares);
      }
      break;
    case 'X':
      builder->partial_cancel(event.ref, event.shares);
      break;
    case 'D':
      builder->remove(event.ref);
      break;
    default:
      builder->replace(event.ref, event.new_ref, event.price, event.shares);
      break;
  }
}

std::string stock_name(const std::array<char, 8>& stock) {
  std::string name(stock.data(), stock.size());
  name.erase(name.find_last_not_of(' ') + 1);
  return name;
}

bool is_order_message(char type) noexcept {
  return type == 'A' || type == 'F' || type == 'E' || type == 'C' || type == 'X' || type == 'D' ||
         type == 'U';
}

}  // namespace llme::workload
