// Property violations in a fuzz target must stop the run with a stack trace (libFuzzer saves the input).
#pragma once

#include <cstdio>
#include <cstdlib>

#define FUZZ_CHECK(cond)                                                          \
  do {                                                                            \
    if (!(cond)) {                                                                \
      std::fprintf(stderr, "FUZZ_CHECK failed: %s (%s:%d)\n", #cond, __FILE__, __LINE__); \
      std::abort();                                                               \
    }                                                                             \
  } while (0)
