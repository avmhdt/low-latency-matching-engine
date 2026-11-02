// Shared scaffolding for the llme-* command-line tools (FR-29): every tool has --help, exits
// non-zero on error and prints a JSON summary on stdout with --json.
#pragma once

#include <CLI/CLI.hpp>
#include <exception>
#include <iostream>
#include <nlohmann/json.hpp>
#include <string>

namespace llme::tools {

// Parses the command line, then runs the tool body.
template <class Body>
int parse_and_run(CLI::App& app, int argc, char** argv, const Body& body) {
  try {
    app.parse(argc, argv);
  } catch (const CLI::ParseError& e) {
    return app.exit(e);
  }
  return body();
}

// Outermost frame of every tool: failures become a message and a non-zero exit.
template <class Run>
int guarded_main(const Run& run) noexcept {
  try {
    return run();
  } catch (const std::exception& e) {
    std::cerr << "error: " << e.what() << '\n';
  } catch (...) {
    std::cerr << "error: unknown exception\n";
  }
  return 1;
}

}  // namespace llme::tools
