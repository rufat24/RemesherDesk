// SHA-256 (FIPS 180-4), for provenance hashes in the worker protocol.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace rdesk {

class Sha256 {
 public:
  Sha256() noexcept;
  void update(const void* data, std::size_t size) noexcept;
  // Finishes the hash; the object must not be updated afterwards.
  std::array<std::uint8_t, 32> digest() noexcept;
  std::string hex_digest();  // 64 lowercase hex characters

 private:
  void block(const std::uint8_t* p) noexcept;
  std::array<std::uint32_t, 8> h_;
  std::array<std::uint8_t, 64> buf_{};
  std::size_t buffered_ = 0;
  std::uint64_t total_ = 0;
};

std::string sha256_hex(const void* data, std::size_t size);

}  // namespace rdesk
