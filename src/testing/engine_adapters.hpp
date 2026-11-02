// Uniform test-side view of every engine, so that golden tests, property tests and llme-diff can
// drive any pair. Type erasure is fine here: this layer never runs on the hot path.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "llme/messages.hpp"
#include "llme/types.hpp"

namespace llme {

class EngineAdapter {
 public:
  EngineAdapter() = default;
  EngineAdapter(const EngineAdapter&) = delete;
  EngineAdapter& operator=(const EngineAdapter&) = delete;
  EngineAdapter(EngineAdapter&&) = delete;
  EngineAdapter& operator=(EngineAdapter&&) = delete;
  virtual ~EngineAdapter() = default;

  virtual void on_message(const InMsg& m, std::vector<OutMsg>& out) = 0;
  [[nodiscard]] virtual BookSnapshot snapshot() const = 0;
  [[nodiscard]] virtual std::uint32_t live_orders() const = 0;
};

[[nodiscard]] std::unique_ptr<EngineAdapter> make_adapter(std::string_view name,
                                                          const EngineConfig& cfg);

// Engine names make_adapter accepts.
[[nodiscard]] std::vector<std::string> adapter_names();

}  // namespace llme
