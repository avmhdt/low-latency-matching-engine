// Streaming XXH3-64 (seed 0) over OutMsg bytes: the deterministic-replay digest of FR-15.
//
// Invariants: the digest depends only on the bytes fed to it, in order. Storage is inline so
// construction after start-up allocates nothing. Thread-safety: one instance per thread.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

#include "llme/messages.hpp"

namespace llme {

class OutputHasher {
 public:
  OutputHasher() noexcept;

  void update(const OutMsg& m) noexcept;
  [[nodiscard]] std::uint64_t digest() const noexcept;

 private:
  // XXH3_state_t is 576 bytes aligned to 64; the .cpp checks that this storage fits it.
  alignas(64) std::array<std::byte, 576> state_;
};

// One-shot XXH3-64 of a byte range, used for input-log hashes.
[[nodiscard]] std::uint64_t xxh3_bytes(std::span<const std::byte> bytes) noexcept;

// "0x" plus 16 lowercase hex digits.
[[nodiscard]] std::string format_hash(std::uint64_t h);

}  // namespace llme
