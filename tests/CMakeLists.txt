include(GoogleTest)

# Adds a gtest binary whose cases all carry one ctest label.
function(llme_add_test name label)
  add_executable(${name} ${ARGN})
  target_link_libraries(${name} PRIVATE llme_io GTest::gtest_main)
  llme_target_defaults(${name})
  gtest_discover_tests(${name} DISCOVERY_MODE PRE_TEST PROPERTIES LABELS ${label}
                       WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
endfunction()

llme_add_test(llme_unit_tests unit
  unit/messages_test.cpp
  unit/mlog_test.cpp
  unit/output_hash_test.cpp)

if(LLME_BUILD_TOOLS)
  add_test(NAME Cli.GenRunRoundTrip
           COMMAND sh "${CMAKE_CURRENT_SOURCE_DIR}/integration/cli_round_trip.sh"
                   $<TARGET_FILE:llme-gen> $<TARGET_FILE:llme-run> "${CMAKE_CURRENT_BINARY_DIR}")
  set_tests_properties(Cli.GenRunRoundTrip PROPERTIES LABELS integration)
endif()
