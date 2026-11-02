#include "llme/output_hash.hpp"

#include <array>
#include <cstdio>

#define XXH_INLINE_ALL
#include <xxhash.h>

namespace llme {

namespace {

XXH3_state_t* as_state(std::array<std::byte, 576>& storage) noexcept {
  static_assert(sizeof(XXH3_state_t) <= 576 && alignof(XXH3_state_t) <= 64);
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): storage sized for the state
  return reinterpret_cast<XXH3_state_t*>(storage.data());
}

const XXH3_state_t* as_state(const std::array<std::byte, 576>& storage) noexcept {
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): storage sized for the state
  return reinterpret_cast<const XXH3_state_t*>(storage.data());
}

}  // namespace

// NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init): XXH3_64bits_reset initialises state_
OutputHasher::OutputHasher() noexcept { XXH3_64bits_reset(as_state(state_)); }

void OutputHasher::update(const OutMsg& m) noexcept {
  XXH3_64bits_update(as_state(state_), &m, sizeof(OutMsg));
}

std::uint64_t OutputHasher::digest() const noexcept { return XXH3_64bits_digest(as_state(state_)); }

std::uint64_t xxh3_bytes(std::span<const std::byte> bytes) noexcept {
  return XXH3_64bits(bytes.data(), bytes.size());
}

std::string format_hash(std::uint64_t h) {
  std::array<char, 19> text{};
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): snprintf is the simple way to format hex
  std::snprintf(text.data(), text.size(), "0x%016llx", static_cast<unsigned long long>(h));
  return {text.data(), text.size() - 1};
}

}  // namespace llme
