function(llme_add_fuzzer name)
  add_executable(${name} ${ARGN})
  target_link_libraries(${name} PRIVATE llme_io)
  target_link_options(${name} PRIVATE -fsanitize=fuzzer)
  llme_target_defaults(${name})
endfunction()

llme_add_fuzzer(fuzz_mlog_reader fuzz_mlog_reader.cpp)
