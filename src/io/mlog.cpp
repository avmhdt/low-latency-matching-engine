#include "llme/mlog.hpp"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <limits>
#include <new>

namespace llme {

std::string_view to_string(MlogError e) noexcept {
  switch (e) {
    case MlogError::OpenFailed:
      return "cannot open file";
    case MlogError::TooSmall:
      return "file smaller than the 64-byte header";
    case MlogError::BadMagic:
      return "bad magic";
    case MlogError::BadVersion:
      return "unsupported version";
    case MlogError::BadRecordSize:
      return "bad record size";
    case MlogError::BadRecordCount:
      return "record count does not match file size";
    case MlogError::ReservedNonZero:
      return "non-zero reserved header bytes";
    case MlogError::Misaligned:
      return "image is not 8-byte aligned";
    case MlogError::MapFailed:
      return "mmap failed";
    case MlogError::WriteFailed:
      return "write failed";
  }
  return "unknown error";
}

expected<const MlogHeader*, MlogError> validate_log(std::span<const std::byte> bytes,
                                                    const std::array<char, 8>& magic,
                                                    std::uint16_t record_size) noexcept {
  if (bytes.size() < sizeof(MlogHeader)) {
    return tl::unexpected(MlogError::TooSmall);
  }
  if (reinterpret_cast<std::uintptr_t>(bytes.data()) %  // NOLINT: alignment test only
          alignof(MlogHeader) !=
      0) {
    return tl::unexpected(MlogError::Misaligned);
  }
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): header of a file image
  const auto* header = reinterpret_cast<const MlogHeader*>(bytes.data());
  if (header->magic != magic) {
    return tl::unexpected(MlogError::BadMagic);
  }
  if (header->version != kLogVersion) {
    return tl::unexpected(MlogError::BadVersion);
  }
  if (header->record_size != record_size) {
    return tl::unexpected(MlogError::BadRecordSize);
  }
  const std::size_t payload = bytes.size() - sizeof(MlogHeader);
  if (payload % record_size != 0 || header->record_count != payload / record_size) {
    return tl::unexpected(MlogError::BadRecordCount);
  }
  if (std::any_of(header->reserved.begin(), header->reserved.end(),
                  [](std::uint8_t b) { return b != 0; })) {
    return tl::unexpected(MlogError::ReservedNonZero);
  }
  return header;
}

namespace detail {

MappedFile::~MappedFile() {
  if (data_ != nullptr) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): munmap takes a non-const pointer
    ::munmap(const_cast<std::byte*>(data_), size_);
  }
}

expected<MappedFile, MlogError> MappedFile::open(const std::filesystem::path& p) {
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): POSIX open
  const int fd = ::open(p.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    return tl::unexpected(MlogError::OpenFailed);
  }
  struct stat st {};
  if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
    ::close(fd);
    return tl::unexpected(MlogError::OpenFailed);
  }
  const auto size = static_cast<std::size_t>(st.st_size);
  if (size < sizeof(MlogHeader)) {
    ::close(fd);
    return tl::unexpected(MlogError::TooSmall);
  }
  void* base = ::mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
  ::close(fd);
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr): MAP_FAILED
  if (base == MAP_FAILED) {
    return tl::unexpected(MlogError::MapFailed);
  }
  ::madvise(base, size, MADV_SEQUENTIAL);
  MappedFile file;
  file.data_ = static_cast<const std::byte*>(base);
  file.size_ = size;
  return file;
}

void FileCloser::operator()(std::FILE* f) const noexcept {
  // NOLINTNEXTLINE(cppcoreguidelines-owning-memory): this is the unique_ptr deleter
  std::fclose(f);
}

expected<RawLogWriter, MlogError> RawLogWriter::create(const std::filesystem::path& p,
                                                       const std::array<char, 8>& magic,
                                                       std::uint16_t record_size,
                                                       const LogMeta& meta) {
  RawLogWriter writer;
  // NOLINTNEXTLINE(cppcoreguidelines-owning-memory): ownership passes to the unique_ptr here
  writer.file_.reset(std::fopen(p.c_str(), "wb"));
  if (!writer.file_) {
    return tl::unexpected(MlogError::OpenFailed);
  }
  constexpr std::size_t kBufferBytes = 1U << 20U;
  writer.buffer_.resize(kBufferBytes);
  std::setvbuf(writer.file_.get(), writer.buffer_.data(), _IOFBF, kBufferBytes);

  writer.header_.magic = magic;
  writer.header_.version = kLogVersion;
  writer.header_.record_size = record_size;
  writer.header_.symbol_id = meta.symbol_id;
  writer.header_.tick_size_e9 = meta.tick_size_e9;
  writer.header_.seed = meta.seed;
  writer.header_.source = to_wire(meta.source);
  if (std::fwrite(&writer.header_, sizeof(MlogHeader), 1, writer.file_.get()) != 1) {
    return tl::unexpected(MlogError::WriteFailed);
  }
  return writer;
}

bool RawLogWriter::write(const void* record) noexcept {
  if (std::fwrite(record, header_.record_size, 1, file_.get()) != 1) {
    return false;
  }
  ++header_.record_count;
  return true;
}

expected<void, MlogError> RawLogWriter::close() noexcept {
  if (!file_) {
    return {};
  }
  const bool ok = std::fseek(file_.get(), 0, SEEK_SET) == 0 &&
                  std::fwrite(&header_, sizeof(MlogHeader), 1, file_.get()) == 1 &&
                  std::fflush(file_.get()) == 0;
  file_.reset();
  if (!ok) {
    return tl::unexpected(MlogError::WriteFailed);
  }
  return {};
}

}  // namespace detail

}  // namespace llme
