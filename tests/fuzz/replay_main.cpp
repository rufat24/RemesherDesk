// Stand-in for libFuzzer's main() in ordinary builds: feeds every file (and every file in every directory) named on the
// command line to the fuzz target once. That turns the seed corpus and the regression inputs into a plain ctest that
// runs on every compiler.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size);

namespace fs = std::filesystem;

int main(int argc, char** argv) {
  std::vector<fs::path> files;
  for (int i = 1; i < argc; ++i) {
    const fs::path p(argv[i]);
    std::error_code ec;
    if (fs::is_directory(p, ec)) {
      for (const auto& e : fs::directory_iterator(p)) {
        if (e.is_regular_file() && e.path().filename().string()[0] != '.') files.push_back(e.path());
      }
    } else if (fs::is_regular_file(p, ec)) {
      files.push_back(p);
    } else {
      std::fprintf(stderr, "replay: no such file or directory: %s\n", argv[i]);
      return 2;
    }
  }
  std::sort(files.begin(), files.end());
  for (const fs::path& f : files) {
    std::ifstream in(f, std::ios::binary);
    const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    LLVMFuzzerTestOneInput(bytes.data(), bytes.size());
  }
  std::printf("replayed %zu input(s)\n", files.size());
  return files.empty() ? 3 : 0;  // an empty corpus is a configuration error, not a pass
}
