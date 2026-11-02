#include "diff.hpp"

#include <algorithm>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace llme {

namespace {

std::string describe(const OutMsg& r) {
  std::ostringstream text;
  text << "{type=" << int{r.type} << " side=" << int{r.side} << " reason=" << int{r.reason}
       << " flags=" << int{r.flags} << " qty=" << r.qty << " seq=" << r.seq << " id=" << r.order_id
       << " contra=" << r.contra_id << " px=" << r.price << " leaves=" << r.leaves << "}";
  return text.str();
}

}  // namespace

DiffRunner::DiffRunner(std::unique_ptr<EngineAdapter> a, std::unique_ptr<EngineAdapter> b)
    : a_(std::move(a)), b_(std::move(b)) {}

bool DiffRunner::step(const InMsg& m) {
  out_a_.clear();
  out_b_.clear();
  a_->on_message(m, out_a_);
  b_->on_message(m, out_b_);
  const std::uint64_t index = stats_.messages++;

  for (const OutMsg& r : out_a_) {
    stats_.fills += r.type == to_wire(OutType::Fill) ? 1U : 0U;
    stats_.rejects += r.type == to_wire(OutType::Reject) ? 1U : 0U;
  }
  if (std::equal(out_a_.begin(), out_a_.end(), out_b_.begin(), out_b_.end())) {
    return full_check_every_ == 0 || check_invariants(index);
  }

  ++stats_.mismatches;
  if (!stats_.first_mismatch_index) {
    stats_.first_mismatch_index = index;
    std::ostringstream text;
    text << "message " << index << ": " << out_a_.size() << " vs " << out_b_.size() << " records";
    const auto [ia, ib] = std::mismatch(out_a_.begin(), out_a_.end(), out_b_.begin(), out_b_.end());
    if (ia != out_a_.end() && ib != out_b_.end()) {
      text << "; first difference " << describe(*ia) << " vs " << describe(*ib);
    }
    stats_.first_mismatch = text.str();
  }
  return false;
}

bool DiffRunner::check_invariants(std::uint64_t index) {
  try {
    a_->check_touched(out_a_);
    if (index % full_check_every_ == 0) {
      a_->check_full();
      ++stats_.full_checks;
    }
    return true;
  } catch (const std::logic_error& e) {
    record_invariant_failure(index, e.what());
    return false;
  }
}

void DiffRunner::record_invariant_failure(std::uint64_t index, const std::string& what) {
  ++stats_.invariant_failures;
  if (!stats_.first_mismatch_index) {
    stats_.first_mismatch_index = index;
    stats_.first_mismatch = "invariant at message " + std::to_string(index) + ": " + what;
  }
}

void DiffRunner::finish() {
  if (full_check_every_ != 0) {
    try {
      a_->check_full();
      ++stats_.full_checks;
    } catch (const std::logic_error& e) {
      record_invariant_failure(stats_.messages, e.what());
    }
  }
  if (a_->snapshot() == b_->snapshot()) {
    return;
  }
  ++stats_.mismatches;
  if (!stats_.first_mismatch_index) {
    stats_.first_mismatch_index = stats_.messages;
    stats_.first_mismatch = "final books differ";
  }
}

std::optional<std::uint64_t> first_mismatch(const std::vector<InMsg>& msgs, const EngineFactory& a,
                                            const EngineFactory& b,
                                            std::uint64_t full_check_every) {
  DiffRunner runner(a(), b());
  runner.enable_invariants(full_check_every);
  for (const InMsg& m : msgs) {
    if (!runner.step(m)) {
      return runner.stats().first_mismatch_index;
    }
  }
  runner.finish();
  return runner.stats().first_mismatch_index;
}

std::vector<InMsg> shrink(std::vector<InMsg> failing,
                          const std::function<bool(const std::vector<InMsg>&)>& still_fails,
                          std::chrono::seconds budget) {
  const auto deadline = std::chrono::steady_clock::now() + budget;
  std::vector<InMsg> candidate;
  for (std::size_t chunk = std::max<std::size_t>(failing.size() / 2, 1);;) {
    std::size_t start = 0;
    while (start < failing.size()) {
      if (std::chrono::steady_clock::now() >= deadline) {
        return failing;
      }
      const std::size_t stop = std::min(start + chunk, failing.size());
      candidate.assign(failing.begin(), failing.begin() + static_cast<std::ptrdiff_t>(start));
      candidate.insert(candidate.end(), failing.begin() + static_cast<std::ptrdiff_t>(stop),
                       failing.end());
      if (still_fails(candidate)) {
        failing.swap(candidate);
      } else {
        start = stop;
      }
    }
    if (chunk == 1) {
      return failing;
    }
    chunk /= 2;
  }
}

}  // namespace llme
