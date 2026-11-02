// Differential harness (FR-17): run two engines side by side on the same messages, compare every
// output record and the final books, and shrink a failing prefix to a minimal reproducer.
#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "engine_adapters.hpp"
#include "llme/messages.hpp"
#include "llme/types.hpp"

namespace llme {

using EngineFactory = std::function<std::unique_ptr<EngineAdapter>()>;

struct DiffStats {
  std::uint64_t messages = 0;
  std::uint64_t fills = 0;
  std::uint64_t rejects = 0;
  std::uint64_t mismatches = 0;
  std::optional<std::uint64_t> first_mismatch_index;  // message index, or messages for final book
  std::string first_mismatch;                         // human-readable description
};

// Feeds messages one at a time and records the first divergence between engine a and engine b.
class DiffRunner {
 public:
  DiffRunner(std::unique_ptr<EngineAdapter> a, std::unique_ptr<EngineAdapter> b);

  // Returns false once a mismatch has been seen; the caller may stop there.
  bool step(const InMsg& m);
  // Compares the final books; call once after the last step.
  void finish();

  [[nodiscard]] const DiffStats& stats() const noexcept { return stats_; }
  [[nodiscard]] const std::vector<OutMsg>& last_output() const noexcept { return out_a_; }
  [[nodiscard]] EngineAdapter& engine_a() noexcept { return *a_; }

 private:
  std::unique_ptr<EngineAdapter> a_;
  std::unique_ptr<EngineAdapter> b_;
  std::vector<OutMsg> out_a_;
  std::vector<OutMsg> out_b_;
  DiffStats stats_;
};

// Index of the first divergence over msgs, or nullopt if the engines agree, final book included.
[[nodiscard]] std::optional<std::uint64_t> first_mismatch(const std::vector<InMsg>& msgs,
                                                          const EngineFactory& a,
                                                          const EngineFactory& b);

// Delta debugging: drop chunks, halving their size, while still_fails holds. Stops early at the
// deadline and returns the smallest failing sequence found so far.
[[nodiscard]] std::vector<InMsg> shrink(
    std::vector<InMsg> failing, const std::function<bool(const std::vector<InMsg>&)>& still_fails,
    std::chrono::seconds budget);

}  // namespace llme
