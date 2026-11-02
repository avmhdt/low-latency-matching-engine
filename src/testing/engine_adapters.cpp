#include "engine_adapters.hpp"

#include <stdexcept>
#include <string>

#include "llme/engine.hpp"
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

// Sink that appends to whatever vector the current on_message call supplied.
struct VectorSink {
  std::vector<OutMsg>** target;

  void operator()(const OutMsg& m) const noexcept { (*target)->push_back(m); }
};

template <LevelStore L>
class FastAdapter final : public EngineAdapter {
 public:
  explicit FastAdapter(const EngineConfig& cfg) : engine_(cfg, VectorSink{&target_}) {}

  void on_message(const InMsg& m, std::vector<OutMsg>& out) override {
    target_ = &out;
    engine_.on_message(m);
  }
  [[nodiscard]] BookSnapshot snapshot() const override { return engine_.snapshot(); }
  [[nodiscard]] std::uint32_t live_orders() const override { return engine_.live_orders(); }

  void check_touched(const std::vector<OutMsg>& out) const override {
    engine_.check_counts();
    for (const OutMsg& r : out) {
      if (r.type == to_wire(OutType::LevelUpdate)) {
        engine_.check_level(static_cast<Side>(r.side), r.price);
      }
    }
  }

  void check_full() const override {
    engine_.check_invariants();
    engine_.check_pool();
  }

 private:
  std::vector<OutMsg>* target_ = nullptr;
  MatchingEngine<L, VectorSink> engine_;
};

}  // namespace

std::unique_ptr<EngineAdapter> make_adapter(std::string_view name, const EngineConfig& cfg) {
  if (name == "ref") {
    return std::make_unique<ReferenceAdapter>(cfg);
  }
  if (name == "d2") {
    return std::make_unique<FastAdapter<LadderLevelStore>>(cfg);
  }
  throw std::invalid_argument("unknown engine: " + std::string(name));
}

std::vector<std::string> adapter_names() { return {"ref", "d2"}; }

}  // namespace llme
