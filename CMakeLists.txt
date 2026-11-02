cmake_minimum_required(VERSION 3.28)
project(low_latency_matching_engine VERSION 0.1.0 LANGUAGES C CXX)

option(LLME_BUILD_TESTS "Build unit, golden, property and integration tests" ON)
option(LLME_BUILD_BENCH "Build benchmarks" ON)
option(LLME_BUILD_FUZZ "Build libFuzzer targets (Clang only)" OFF)
option(LLME_BUILD_TOOLS "Build llme-* command-line tools" ON)
option(LLME_COVERAGE "Instrument for llvm-cov source-based coverage (Clang only)" OFF)
set(LLME_SANITIZE "" CACHE STRING "Sanitizer list, e.g. address,undefined or thread")
set(LLME_CLANG_TIDY "" CACHE STRING "clang-tidy command applied to first-party targets")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(CompilerWarnings)
include(Sanitizers)
include(Dependencies)

add_library(llme_headers INTERFACE)
target_include_directories(llme_headers INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(llme_headers INTERFACE tl::expected)

add_library(llme_io STATIC src/io/mlog.cpp src/io/output_hash.cpp)
target_link_libraries(llme_io PUBLIC llme_headers PRIVATE llme_xxhash)
llme_target_defaults(llme_io)

add_library(llme_core STATIC src/core/order_book.cpp)
target_link_libraries(llme_core PUBLIC llme_headers)
llme_target_defaults(llme_core)

add_library(llme_reference STATIC src/reference/reference_engine.cpp)
target_link_libraries(llme_reference PUBLIC llme_headers)
llme_target_defaults(llme_reference)

add_library(llme_pipeline STATIC
  src/pipeline/pipeline.cpp src/pipeline/gateway.cpp src/pipeline/publisher.cpp)
target_link_libraries(llme_pipeline PUBLIC llme_io llme_core)
find_package(Threads REQUIRED)
target_link_libraries(llme_pipeline PUBLIC Threads::Threads)
llme_target_defaults(llme_pipeline)

add_library(llme_testing STATIC src/testing/engine_adapters.cpp src/testing/diff.cpp)
target_include_directories(llme_testing PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src/testing")
target_link_libraries(llme_testing PUBLIC llme_headers PRIVATE llme_core llme_reference)
llme_target_defaults(llme_testing)

if(LLME_BUILD_TOOLS OR LLME_BUILD_TESTS)
  add_library(llme_workload STATIC src/workload/generator.cpp src/workload/profile.cpp)
  target_include_directories(llme_workload PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src/workload")
  target_link_libraries(llme_workload PUBLIC llme_headers llme_reference
                                      PRIVATE tomlplusplus::tomlplusplus)
  # DQ-08: no contraction, so the same arithmetic gives the same bits with and without FMA.
  target_compile_options(llme_workload PRIVATE -ffp-contract=off)
  llme_target_defaults(llme_workload)
endif()

if(LLME_BUILD_TOOLS)
  add_subdirectory(src/tools)
endif()

if(LLME_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(LLME_BUILD_BENCH)
  add_subdirectory(bench)
endif()

if(LLME_BUILD_FUZZ)
  add_subdirectory(fuzz)
endif()

# Licence metadata (D-10). The SPDX id is MIT; the licence covers code only, never data.
set(CPACK_PACKAGE_NAME "low-latency-matching-engine")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
set(CPACK_RPM_PACKAGE_LICENSE "MIT")
include(CPack)
