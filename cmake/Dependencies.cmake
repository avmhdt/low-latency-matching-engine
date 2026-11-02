# Every third-party dependency, pinned to an exact tag. Versions are recorded in docs/decisions.md.
if(NOT DEFINED ENV{CPM_SOURCE_CACHE} AND NOT DEFINED CPM_SOURCE_CACHE)
  set(CPM_SOURCE_CACHE "$ENV{HOME}/.cache/CPM")
endif()
include(CPM)

CPMAddPackage(
  NAME expected
  GITHUB_REPOSITORY TartanLlama/expected
  GIT_TAG v1.1.0
  SYSTEM YES
  OPTIONS "EXPECTED_BUILD_TESTS OFF" "EXPECTED_BUILD_PACKAGE OFF")

CPMAddPackage(
  NAME xxHash
  GITHUB_REPOSITORY Cyan4973/xxHash
  GIT_TAG v0.8.3
  DOWNLOAD_ONLY YES)
add_library(llme_xxhash INTERFACE)
target_include_directories(llme_xxhash SYSTEM INTERFACE "${xxHash_SOURCE_DIR}")

if(LLME_BUILD_TOOLS OR LLME_BUILD_TESTS)
  CPMAddPackage(
    NAME CLI11
    GITHUB_REPOSITORY CLIUtils/CLI11
    GIT_TAG v2.4.2
    SYSTEM YES
    OPTIONS "CLI11_BUILD_TESTS OFF" "CLI11_BUILD_EXAMPLES OFF" "CLI11_BUILD_DOCS OFF")
  CPMAddPackage(
    NAME tomlplusplus
    GITHUB_REPOSITORY marzer/tomlplusplus
    GIT_TAG v3.4.0
    SYSTEM YES)
  CPMAddPackage(
    NAME nlohmann_json
    GITHUB_REPOSITORY nlohmann/json
    GIT_TAG v3.11.3
    SYSTEM YES
    OPTIONS "JSON_BuildTests OFF" "JSON_Install OFF")
endif()

if(LLME_BUILD_TESTS)
  CPMAddPackage(
    NAME googletest
    GITHUB_REPOSITORY google/googletest
    GIT_TAG v1.15.2
    SYSTEM YES
    OPTIONS "INSTALL_GTEST OFF" "BUILD_GMOCK OFF")
endif()

if(LLME_BUILD_BENCH)
  CPMAddPackage(
    NAME benchmark
    GITHUB_REPOSITORY google/benchmark
    GIT_TAG v1.9.1
    SYSTEM YES
    OPTIONS "BENCHMARK_ENABLE_TESTING OFF" "BENCHMARK_ENABLE_INSTALL OFF"
            "BENCHMARK_ENABLE_GTEST_TESTS OFF" "BENCHMARK_ENABLE_WERROR OFF")
  CPMAddPackage(
    NAME HdrHistogram_c
    GITHUB_REPOSITORY HdrHistogram/HdrHistogram_c
    GIT_TAG 0.11.8
    SYSTEM YES
    OPTIONS "HDR_HISTOGRAM_BUILD_PROGRAMS OFF" "HDR_HISTOGRAM_BUILD_SHARED OFF"
            "HDR_LOG_REQUIRED OFF")
endif()
