// Minimal Nasdaq TotalView-ITCH 5.0 decoder (FR-21): the framing, the order messages A, F, E, C,
// X, D, U, the stock directory R, and just enough of P, Q and H to count what replay skips.
//
// Wire format: 2-byte big-endian length, then the message. Fields are big-endian at fixed offsets
// from the ITCH 5.0 specification. Any byte sequence is safe input: a wrong length for a known
// type or a bad side byte makes the message malformed, and a short final frame ends the stream.
// Thread-safety: one instance per thread. Tools only; never on the hot path.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "llme/types.hpp"
#include "replay.hpp"

namespace llme::workload {

struct ItchEvent {
  char type = 0;
  std::uint16_t locate = 0;
  std::int64_t timestamp_ns = 0;  // since midnight ET
  OrderId ref = 0;
  OrderId new_ref = 0;
  Side side = Side::Buy;
  std::uint32_t shares = 0;
  std::uint32_t price = 0;  // USD x 10^4
  bool printable = true;
  std::array<char, 8> stock{};
};

// False for a type this decoder ignores or a malformed message; malformed says which.
[[nodiscard]] bool parse_itch(std::span<const std::uint8_t> message, ItchEvent& out,
                              bool& malformed) noexcept;

class ByteSource {
 public:
  ByteSource() = default;
  ByteSource(const ByteSource&) = delete;
  ByteSource& operator=(const ByteSource&) = delete;
  ByteSource(ByteSource&&) = delete;
  ByteSource& operator=(ByteSource&&) = delete;
  virtual ~ByteSource() = default;

  // Returns the bytes read; 0 means end of input.
  virtual std::size_t read(std::uint8_t* out, std::size_t size) = 0;
};

class MemorySource final : public ByteSource {
 public:
  explicit MemorySource(std::span<const std::uint8_t> bytes) noexcept : bytes_(bytes) {}
  std::size_t read(std::uint8_t* out, std::size_t size) override;

 private:
  std::span<const std::uint8_t> bytes_;
};

// Reads a .gz file, or a plain one: zlib passes uncompressed input through unchanged.
class FileSource final : public ByteSource {
 public:
  explicit FileSource(const std::filesystem::path& path);
  ~FileSource() override;
  FileSource(const FileSource&) = delete;
  FileSource& operator=(const FileSource&) = delete;
  FileSource(FileSource&&) = delete;
  FileSource& operator=(FileSource&&) = delete;

  std::size_t read(std::uint8_t* out, std::size_t size) override;

 private:
  void* file_;
};

// Splits the stream into frames. Each returned span is valid until the next call.
class ItchFrameReader {
 public:
  explicit ItchFrameReader(ByteSource& source);

  [[nodiscard]] bool next(std::span<const std::uint8_t>& message);
  [[nodiscard]] bool truncated() const noexcept { return truncated_; }

 private:
  [[nodiscard]] bool ensure(std::size_t bytes);

  ByteSource* source_;
  std::vector<std::uint8_t> buffer_;
  std::size_t start_ = 0;
  std::size_t end_ = 0;
  bool truncated_ = false;
};

// Routes decoded events to the builder of each selected stock, applying DQ-05.
class ItchConverter {
 public:
  ItchConverter();

  void select(std::uint16_t locate, ReplayBuilder* builder) noexcept;
  void on_event(const ItchEvent& event);

 private:
  std::vector<ReplayBuilder*> builders_;
};

// "AAPL    " to "AAPL".
[[nodiscard]] std::string stock_name(const std::array<char, 8>& stock);

// True for the seven order messages, the ones D-05 ranks symbols by.
[[nodiscard]] bool is_order_message(char type) noexcept;

}  // namespace llme::workload
