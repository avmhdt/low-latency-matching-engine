#include "alloc_hook.hpp"

#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <new>

extern "C" {
void* __libc_malloc(std::size_t size);
void* __libc_calloc(std::size_t count, std::size_t size);
void* __libc_realloc(void* ptr, std::size_t size);
void* __libc_memalign(std::size_t alignment, std::size_t size);
void __libc_free(void* ptr);
}

namespace {

std::atomic<bool> g_armed{false};
std::atomic<std::uint64_t> g_allocations{0};

inline void note_allocation() noexcept {
  if (g_armed.load(std::memory_order_relaxed)) {
    g_allocations.fetch_add(1, std::memory_order_relaxed);
  }
}

void* counted_new(std::size_t size) {
  note_allocation();
  void* ptr = __libc_malloc(size == 0 ? 1 : size);
  if (ptr == nullptr) {
    throw std::bad_alloc();
  }
  return ptr;
}

void* counted_new_aligned(std::size_t size, std::align_val_t alignment) {
  note_allocation();
  void* ptr = __libc_memalign(static_cast<std::size_t>(alignment), size == 0 ? 1 : size);
  if (ptr == nullptr) {
    throw std::bad_alloc();
  }
  return ptr;
}

}  // namespace

namespace llme::testing {

void arm_alloc_hook() noexcept {
  g_allocations.store(0, std::memory_order_relaxed);
  g_armed.store(true, std::memory_order_seq_cst);
}

void disarm_alloc_hook() noexcept { g_armed.store(false, std::memory_order_seq_cst); }

std::uint64_t allocations_while_armed() noexcept {
  return g_allocations.load(std::memory_order_relaxed);
}

}  // namespace llme::testing

extern "C" {

void* malloc(std::size_t size) {
  note_allocation();
  return __libc_malloc(size);
}

void* calloc(std::size_t count, std::size_t size) {
  note_allocation();
  return __libc_calloc(count, size);
}

void* realloc(void* ptr, std::size_t size) {
  note_allocation();
  return __libc_realloc(ptr, size);
}

void* memalign(std::size_t alignment, std::size_t size) {
  note_allocation();
  return __libc_memalign(alignment, size);
}

void* aligned_alloc(std::size_t alignment, std::size_t size) {
  note_allocation();
  return __libc_memalign(alignment, size);
}

int posix_memalign(void** out, std::size_t alignment, std::size_t size) {
  note_allocation();
  *out = __libc_memalign(alignment, size);
  return *out == nullptr ? 12 : 0;  // ENOMEM
}

void free(void* ptr) { __libc_free(ptr); }

}  // extern "C"

void* operator new(std::size_t size) { return counted_new(size); }
void* operator new[](std::size_t size) { return counted_new(size); }
void* operator new(std::size_t size, std::align_val_t alignment) {
  return counted_new_aligned(size, alignment);
}
void* operator new[](std::size_t size, std::align_val_t alignment) {
  return counted_new_aligned(size, alignment);
}
void operator delete(void* ptr) noexcept { __libc_free(ptr); }
void operator delete[](void* ptr) noexcept { __libc_free(ptr); }
void operator delete(void* ptr, std::size_t /*size*/) noexcept { __libc_free(ptr); }
void operator delete[](void* ptr, std::size_t /*size*/) noexcept { __libc_free(ptr); }
void operator delete(void* ptr, std::align_val_t /*alignment*/) noexcept { __libc_free(ptr); }
void operator delete[](void* ptr, std::align_val_t /*alignment*/) noexcept { __libc_free(ptr); }
void operator delete(void* ptr, std::size_t /*size*/, std::align_val_t /*alignment*/) noexcept {
  __libc_free(ptr);
}
void operator delete[](void* ptr, std::size_t /*size*/, std::align_val_t /*alignment*/) noexcept {
  __libc_free(ptr);
}
