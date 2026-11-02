// Readers and writers for .mlog (InMsg) and .olog (OutMsg) files.
//
// A log is an MlogHeader followed by record_count fixed-size records. Readers memory-map the
// file, validate it completely on open and report failures as values, never by throwing, so the
// read path stays usable from the gateway. Writers buffer through stdio and patch record_count
// on close. Thread-safety: an instance belongs to one thread; a reader's span may be read from
// any thread while the reader is alive.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <span>
#include <string_view>
#include <tl/expected.hpp>
#include <utility>
#include <vector>

#include "llme/messages.hpp"

namespace llme {

template <class T, class E>
using expected = tl::expected<T, E>;

enum class MlogError : std::uint8_t {
  OpenFailed,
  TooSmall,
  BadMagic,
  BadVersion,
  BadRecordSize,
  BadRecordCount,
  ReservedNonZero,
  Misaligned,
  MapFailed,
  WriteFailed
};

[[nodiscard]] std::string_view to_string(MlogError e) noexcept;

// Checks every header rule of FR-01 and DQ-09 against a complete file image.
[[nodiscard]] expected<const MlogHeader*, MlogError> validate_log(
    std::span<const std::byte> bytes, const std::array<char, 8>& magic,
    std::uint16_t record_size) noexcept;

namespace detail {

// Read-only private mapping; movable, unmaps on destruction.
class MappedFile {
 public:
  MappedFile() = default;
  MappedFile(const MappedFile&) = delete;
  MappedFile& operator=(const MappedFile&) = delete;
  MappedFile(MappedFile&& other) noexcept
      : data_(std::exchange(other.data_, nullptr)), size_(std::exchange(other.size_, 0)) {}
  MappedFile& operator=(MappedFile&& other) noexcept {
    std::swap(data_, other.data_);
    std::swap(size_, other.size_);
    return *this;
  }
  ~MappedFile();

  [[nodiscard]] static expected<MappedFile, MlogError> open(const std::filesystem::path& p);
  [[nodiscard]] std::span<const std::byte> bytes() const noexcept { return {data_, size_}; }

 private:
  const std::byte* data_ = nullptr;
  std::size_t size_ = 0;
};

template <class Record>
struct LogTraits;
template <>
struct LogTraits<InMsg> {
  static constexpr const std::array<char, 8>& kMagic = kMlogMagic;
};
template <>
struct LogTraits<OutMsg> {
  static constexpr const std::array<char, 8>& kMagic = kOlogMagic;
};

}  // namespace detail

template <class Record>
class LogReader {
 public:
  [[nodiscard]] static expected<LogReader, MlogError> open(const std::filesystem::path& p) {
    auto file = detail::MappedFile::open(p);
    if (!file) {
      return tl::unexpected(file.error());
    }
    auto view = from_bytes(file->bytes());
    if (view) {
      view->file_ = std::move(*file);
    }
    return view;
  }

  // Validates a caller-owned image, which must outlive the reader and be 8-byte aligned.
  [[nodiscard]] static expected<LogReader, MlogError> from_bytes(
      std::span<const std::byte> bytes) noexcept {
    auto header = validate_log(bytes, detail::LogTraits<Record>::kMagic, sizeof(Record));
    if (!header) {
      return tl::unexpected(header.error());
    }
    LogReader reader;
    reader.header_ = *header;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): validated file image
    reader.records_ = {reinterpret_cast<const Record*>(bytes.data() + sizeof(MlogHeader)),
                       (*header)->record_count};
    return reader;
  }

  [[nodiscard]] const MlogHeader& header() const noexcept { return *header_; }
  [[nodiscard]] std::span<const Record> records() const noexcept { return records_; }

 private:
  LogReader() = default;

  detail::MappedFile file_;
  const MlogHeader* header_ = nullptr;
  std::span<const Record> records_;
};

using MlogReader = LogReader<InMsg>;
using OlogReader = LogReader<OutMsg>;

struct LogMeta {
  std::uint32_t symbol_id = 0;
  std::int64_t tick_size_e9 = 10000000;  // $0.01
  std::uint64_t seed = 0;
  LogSource source = LogSource::Synthetic;
};

namespace detail {

struct FileCloser {
  void operator()(std::FILE* f) const noexcept;
};

class RawLogWriter {
 public:
  [[nodiscard]] static expected<RawLogWriter, MlogError> create(const std::filesystem::path& p,
                                                                const std::array<char, 8>& magic,
                                                                std::uint16_t record_size,
                                                                const LogMeta& meta);
  [[nodiscard]] bool write(const void* record) noexcept;
  // Patches record_count and flushes; a writer that was never closed leaves an invalid file.
  [[nodiscard]] expected<void, MlogError> close() noexcept;

 private:
  std::unique_ptr<std::FILE, FileCloser> file_;
  std::vector<char> buffer_;  // setvbuf storage, must outlive file_
  MlogHeader header_;
};

}  // namespace detail

template <class Record>
class LogWriter {
 public:
  [[nodiscard]] static expected<LogWriter, MlogError> create(const std::filesystem::path& p,
                                                             const LogMeta& meta = {}) {
    auto raw =
        detail::RawLogWriter::create(p, detail::LogTraits<Record>::kMagic, sizeof(Record), meta);
    if (!raw) {
      return tl::unexpected(raw.error());
    }
    return LogWriter(std::move(*raw));
  }

  [[nodiscard]] bool write(const Record& r) noexcept { return raw_.write(&r); }
  [[nodiscard]] expected<void, MlogError> close() noexcept { return raw_.close(); }

 private:
  explicit LogWriter(detail::RawLogWriter raw) : raw_(std::move(raw)) {}

  detail::RawLogWriter raw_;
};

using MlogWriter = LogWriter<InMsg>;
using OlogWriter = LogWriter<OutMsg>;

}  // namespace llme
