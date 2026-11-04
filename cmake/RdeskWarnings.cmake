# Project-wide warning / sanitizer settings, applied per target via rdesk_target_defaults().
# Deliberately NOT applied to vendored third-party code (third_party/), which must be
# compiled with the flags its original build used (parity, see docs/testing.md).
function(rdesk_target_defaults tgt)
  if(MSVC)
    target_compile_options(${tgt} PRIVATE /W4 /permissive- /utf-8 /fp:precise)
    if(RDESK_WERROR)
      target_compile_options(${tgt} PRIVATE /WX)
    endif()
  else()
    target_compile_options(${tgt} PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow
                                          -ffp-contract=off)
    if(RDESK_WERROR)
      target_compile_options(${tgt} PRIVATE -Werror)
    endif()
    if(RDESK_SANITIZE)
      target_compile_options(${tgt} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer
                                            -fno-sanitize-recover=all)
      target_link_options(${tgt} PRIVATE -fsanitize=address,undefined)
    endif()
  endif()
endfunction()
