#include "diff.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <vector>

#include "engine_adapters.hpp"
#include "generator.hpp"

namespace llme {
namespace {

EngineConfig small_config() { return {.price_min = 0, .price_max = 100, .max_orders = 64}; }

// A reference engine with one planted defect: a FILL of exactly `broken_qty` lots reports one
// lot less, as a stand-in for a fast-engine bug.
class BrokenAdapter final : public EngineAdapter {
 public:
  explicit BrokenAdapter(const EngineConfig& cfg, Qty broken_qty)
      : inner_(make_adapter("ref", cfg)), broken_qty_(broken_qty) {}

  void on_message(const InMsg& m, std::vector<OutMsg>& out) override {
    inner_->on_message(m, out);
    for (OutMsg& r : out) {
      if (r.type == to_wire(OutType::Fill) && r.qty == broken_qty_) {
        --r.qty;
      }
    }
  }
  [[nodiscard]] BookSnapshot snapshot() const override { return inner_->snapshot(); }
  [[nodiscard]] std::uint32_t live_orders() const override { return inner_->live_orders(); }

 private:
  std::unique_ptr<EngineAdapter> inner_;
  Qty broken_qty_;
};

// Differs only in its final book: reports one lot fewer at the first level.
class BookOnlyBrokenAdapter final : public EngineAdapter {
 public:
  explicit BookOnlyBrokenAdapter(const EngineConfig& cfg) : inner_(make_adapter("ref", cfg)) {}

  void on_message(const InMsg& m, std::vector<OutMsg>& out) override { inner_->on_message(m, out); }
  [[nodiscard]] BookSnapshot snapshot() const override {
    auto snap = inner_->snapshot();
    if (!snap.bids.empty()) {
      --snap.bids[0].qty;
    }
    return snap;
  }
  [[nodiscard]] std::uint32_t live_orders() const override { return inner_->live_orders(); }

 private:
  std::unique_ptr<EngineAdapter> inner_;
};

std::vector<InMsg> flow(std::uint64_t ops) {
  auto profile = workload::load_profile("configs/workloads/adversarial.toml");
  workload::Generator generator(profile, 4);
  std::vector<InMsg> msgs;
  for (std::uint64_t i = 0; i < ops; ++i) {
    msgs.push_back(generator.next());
  }
  return msgs;
}

TEST(DiffTest, ReferenceAgreesWithItself) {
  const auto profile = workload::load_profile("configs/workloads/adversarial.toml");
  const EngineFactory factory = [&] { return make_adapter("ref", profile.engine); };
  EXPECT_FALSE(first_mismatch(flow(20000), factory, factory).has_value());
}

TEST(DiffTest, FindsAndDescribesARecordDifference) {
  DiffRunner runner(std::make_unique<BrokenAdapter>(small_config(), 3),
                    make_adapter("ref", small_config()));
  InMsg add;
  add.type = to_wire(MsgType::New);
  add.side = to_wire(Side::Buy);
  add.qty = 3;
  add.order_id = 1;
  add.price = 50;
  EXPECT_TRUE(runner.step(add));

  InMsg hit = add;
  hit.side = to_wire(Side::Sell);
  hit.order_id = 2;
  EXPECT_FALSE(runner.step(hit));
  EXPECT_EQ(runner.stats().first_mismatch_index, 1U);
  EXPECT_EQ(runner.stats().mismatches, 1U);
  EXPECT_NE(runner.stats().first_mismatch.find("first difference"), std::string::npos);
}

TEST(DiffTest, ComparesFinalBooks) {
  DiffRunner runner(std::make_unique<BookOnlyBrokenAdapter>(small_config()),
                    make_adapter("ref", small_config()));
  InMsg add;
  add.type = to_wire(MsgType::New);
  add.qty = 3;
  add.order_id = 1;
  add.price = 50;
  EXPECT_TRUE(runner.step(add));
  runner.finish();
  EXPECT_EQ(runner.stats().mismatches, 1U);
  EXPECT_EQ(runner.stats().first_mismatch, "final books differ");
}

TEST(DiffTest, ShrinkFindsAMinimalReproducer) {
  const auto msgs = flow(3000);
  const auto profile = workload::load_profile("configs/workloads/adversarial.toml");
  const EngineFactory broken = [&] { return std::make_unique<BrokenAdapter>(profile.engine, 3); };
  const EngineFactory reference = [&] { return make_adapter("ref", profile.engine); };
  ASSERT_TRUE(first_mismatch(msgs, broken, reference).has_value());

  const auto minimal = shrink(
      msgs,
      [&](const std::vector<InMsg>& candidate) {
        return first_mismatch(candidate, broken, reference).has_value();
      },
      std::chrono::seconds(60));
  // A fill of exactly 3 lots needs one resting order and one aggressor, or one more if the
  // 3 lots are built from a modify; nothing else is required.
  EXPECT_LE(minimal.size(), 3U);
  EXPECT_TRUE(first_mismatch(minimal, broken, reference).has_value());
}

TEST(DiffTest, ShrinkRespectsItsDeadline) {
  const auto msgs = flow(100);
  const auto result =
      shrink(msgs, [](const std::vector<InMsg>&) { return true; }, std::chrono::seconds(0));
  EXPECT_EQ(result.size(), msgs.size());
}

TEST(EngineAdapterTest, UnknownNameThrows) {
  EXPECT_THROW((void)make_adapter("nosuch", small_config()), std::invalid_argument);
  EXPECT_FALSE(adapter_names().empty());
}

}  // namespace
}  // namespace llme
