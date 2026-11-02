# Warning policy (spec section 4) plus per-target defaults for first-party code.
function(llme_target_defaults target)
  target_compile_options(${target} PRIVATE
    -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -Werror)
  if(LLME_CLANG_TIDY)
    set_target_properties(${target} PROPERTIES
      CXX_CLANG_TIDY "${LLME_CLANG_TIDY};--quiet;--warnings-as-errors=*")
  endif()
endfunction()
