# Applied globally, before dependencies, so third-party code is instrumented too.
if(LLME_SANITIZE)
  add_compile_options(-fsanitize=${LLME_SANITIZE} -fno-sanitize-recover=all
                      -fno-omit-frame-pointer -g)
  add_link_options(-fsanitize=${LLME_SANITIZE} -fno-sanitize-recover=all)
endif()

if(LLME_BUILD_FUZZ)
  add_compile_options(-fsanitize=fuzzer-no-link)
endif()

if(LLME_COVERAGE)
  add_compile_options(-fprofile-instr-generate -fcoverage-mapping)
  add_link_options(-fprofile-instr-generate)
endif()
