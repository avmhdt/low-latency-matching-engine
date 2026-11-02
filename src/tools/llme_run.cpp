// llme-run: run one log through an engine. M1 skeleton: validates and hashes the input log;
// engines arrive in M2 and M3, the pipeline in M4.
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>

#include "cli_common.hpp"
#include "llme/mlog.hpp"
#include "llme/output_hash.hpp"

namespace {

int run(int argc, char** argv) {
  CLI::App app{"Run an input log through an engine", "llme-run"};
  std::string in;
  bool json = false;
  app.add_option("--in", in, "Input .mlog path")->required();
  app.add_flag("--json", json, "Print a JSON summary");

  return llme::tools::parse_and_run(app, argc, argv, [&] {
    auto reader = llme::MlogReader::open(in);
    if (!reader) {
      throw std::runtime_error(std::string(llme::to_string(reader.error())) + ": " + in);
    }
    const auto input_hash = llme::format_hash(llme::xxh3_bytes(std::as_bytes(reader->records())));
    if (json) {
      std::cout << nlohmann::json{{"input", in},
                                  {"msgs", reader->header().record_count},
                                  {"input_hash", input_hash}}
                       .dump()
                << '\n';
    } else {
      std::cout << reader->header().record_count << " records, input hash " << input_hash << '\n';
    }
    return 0;
  });
}

}  // namespace

int main(int argc, char** argv) {
  return llme::tools::guarded_main([&] { return run(argc, argv); });
}
