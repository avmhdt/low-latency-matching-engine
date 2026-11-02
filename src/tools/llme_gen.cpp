// llme-gen: synthetic order flow to .mlog. M1 stub: add/cancel pairs, replaced by the full
// generator in M2.
#include <cstdint>
#include <stdexcept>
#include <string>

#include "cli_common.hpp"
#include "llme/mlog.hpp"

namespace {

void write_stub_log(const std::string& out, std::uint64_t ops, std::uint64_t seed) {
  auto writer = llme::MlogWriter::create(out, {.seed = seed});
  if (!writer) {
    throw std::runtime_error(std::string(llme::to_string(writer.error())) + ": " + out);
  }
  for (std::uint64_t i = 0; i < ops; ++i) {
    llme::InMsg m;
    m.order_id = i / 2 + 1;
    if (i % 2 == 0) {
      m.type = llme::to_wire(llme::MsgType::New);
      m.side = llme::to_wire((i / 2) % 2 == 0 ? llme::Side::Buy : llme::Side::Sell);
      m.qty = 1 + static_cast<llme::Qty>(i % 7);
      m.price = 500000 + static_cast<llme::Price>(i % 16);
    } else {
      m.type = llme::to_wire(llme::MsgType::Cancel);
    }
    if (!writer->write(m)) {
      throw std::runtime_error("write failed: " + out);
    }
  }
  if (auto closed = writer->close(); !closed) {
    throw std::runtime_error(std::string(llme::to_string(closed.error())) + ": " + out);
  }
}

int run(int argc, char** argv) {
  CLI::App app{"Generate a synthetic .mlog input log", "llme-gen"};
  std::string profile;
  std::string out;
  std::uint64_t ops = 10000;
  std::uint64_t seed = 42;
  bool json = false;
  app.add_option("--profile", profile, "Workload profile TOML");
  app.add_option("--ops", ops, "Number of messages");
  app.add_option("--seed", seed, "Generator seed");
  app.add_option("--out", out, "Output .mlog path")->required();
  app.add_flag("--json", json, "Print a JSON summary");

  return llme::tools::parse_and_run(app, argc, argv, [&] {
    write_stub_log(out, ops, seed);
    if (json) {
      std::cout << nlohmann::json{{"out", out}, {"ops", ops}, {"seed", seed}}.dump() << '\n';
    }
    return 0;
  });
}

}  // namespace

int main(int argc, char** argv) {
  return llme::tools::guarded_main([&] { return run(argc, argv); });
}
