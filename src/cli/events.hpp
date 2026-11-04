// The worker's stdout: one JSON object per line, flushed per line (docs/architecture.md §4).
#pragma once

#include <chrono>
#include <cstdio>
#include <mutex>
#include <string>

#include <nlohmann/json.hpp>

namespace rdesk::cli {

class EventWriter {
 public:
  explicit EventWriter(std::FILE* out) : out_(out), start_(std::chrono::steady_clock::now()) {}

  // Adds "v":1, "t" and "ms" (milliseconds since the writer was created), writes one line, flushes.
  void emit(const char* type, nlohmann::json fields = nlohmann::json::object()) {
    fields["v"] = 1;
    fields["t"] = type;
    fields["ms"] = elapsed_ms();
    const std::string line = fields.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace) + "\n";
    std::lock_guard<std::mutex> lock(m_);
    std::fwrite(line.data(), 1, line.size(), out_);
    std::fflush(out_);
  }

  long long elapsed_ms() const {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start_).count();
  }

 private:
  std::FILE* out_;
  std::chrono::steady_clock::time_point start_;
  std::mutex m_;
};

}  // namespace rdesk::cli
