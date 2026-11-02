// Test-only allocation counter (FR-12). Linking alloc_hook.cpp replaces global operator new/delete
// and the malloc family for the whole binary, so it must not share a binary with a sanitizer.
//
// Allocations are counted only while armed. Frees are not counted: a hot path that frees memory
// it did not allocate on that path is already caught by the allocation that preceded it.
// Thread-safety: all functions may be called from any thread.
#pragma once

#include <cstdint>

namespace llme::testing {

void arm_alloc_hook() noexcept;
void disarm_alloc_hook() noexcept;
[[nodiscard]] std::uint64_t allocations_while_armed() noexcept;

}  // namespace llme::testing
