// PCG32 (XSH-RR), M. E. O'Neill, https://www.pcg-random.org (Apache-2.0 / MIT reference code, re-implemented here).
// Used instead of std::*_distribution, whose output differs between standard libraries even for the same
// seed (docs/testing.md §4). Everything below is integer arithmetic plus one exact int->double conversion.
#pragma once

#include <cstdint>

namespace rdesk::detail {

class Pcg32 {
 public:
  explicit Pcg32(std::uint64_t seed, std::uint64_t stream = 54) noexcept {
    inc_ = (stream << 1u) | 1u;
    next();
    state_ += seed;
    next();
  }
  std::uint32_t next() noexcept {
    const std::uint64_t old = state_;
    state_ = old * 6364136223846793005ull + inc_;
    const auto xorshifted = static_cast<std::uint32_t>(((old >> 18u) ^ old) >> 27u);
    const auto rot = static_cast<std::uint32_t>(old >> 59u);
    return (xorshifted >> rot) | (xorshifted << ((0u - rot) & 31u));
  }
  // Uniform in [0, 1) with 53 random bits (the "genrand_res53" construction).
  double next_double() noexcept {
    const std::uint64_t a = next() >> 5u, b = next() >> 6u;
    return static_cast<double>((a << 26u) | b) * (1.0 / 9007199254740992.0);
  }

 private:
  std::uint64_t state_ = 0;
  std::uint64_t inc_ = 0;
};

}  // namespace rdesk::detail
