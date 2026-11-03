function(llme_add_fuzzer name)
  add_executable(${name} ${ARGN})
  target_link_libraries(${name} PRIVATE llme_io llme_core llme_reference llme_replay
                                         llme_testing)
  target_link_options(${name} PRIVATE -fsanitize=fuzzer)
  llme_target_defaults(${name})
endfunction()

llme_add_fuzzer(fuzz_mlog_reader fuzz_mlog_reader.cpp)
llme_add_fuzzer(fuzz_engine_diff fuzz_engine_diff.cpp)
llme_add_fuzzer(fuzz_lobster_parser fuzz_lobster_parser.cpp)
llme_add_fuzzer(fuzz_itch_decoder fuzz_itch_decoder.cpp)
