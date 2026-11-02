#include "engine_adapters.hpp"

#include <stdexcept>
#include <string>

#include "llme/reference_engine.hpp"

namespace llme {

namespace {

class ReferenceAdapter final : public EngineAdapter {
 public:
  explicit ReferenceAdapter(const EngineConfig& cfg) : engine_(cfg) {}

  void on_message(const InMsg& m, std::vector<OutMsg>& out) override { engine_.on_message(m, out); }
  [[nodiscard]] BookSnapshot snapshot() const override { return engine_.snapshot(); }
  [[nodiscard]] std::uint32_t live_orders() const override { return engine_.live_orders(); }

 private:
  ReferenceEngine engine_;
};

}  // namespace

std::unique_ptr<EngineAdapter> make_adapter(std::string_view name, const EngineConfig& cfg) {
  if (name == "ref") {
    return std::make_unique<ReferenceAdapter>(cfg);
  }
  throw std::invalid_argument("unknown engine: " + std::string(name));
}

std::vector<std::string> adapter_names() { return {"ref"}; }

}  // namespace llme
