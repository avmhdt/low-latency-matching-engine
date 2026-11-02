// llme-diff: differential harness. M1 skeleton: the option surface only; M2 adds the engines.
#include <stdexcept>
#include <string>

#include "cli_common.hpp"

namespace {

int run(int argc, char** argv) {
  CLI::App app{"Compare engines record by record", "llme-diff"};
  std::string campaign;
  app.add_option("--campaign", campaign, "Campaign TOML");

  return llme::tools::parse_and_run(app, argc, argv,
                                    []() -> int { throw std::runtime_error("not implemented"); });
}

}  // namespace

int main(int argc, char** argv) {
  return llme::tools::guarded_main([&] { return run(argc, argv); });
}
