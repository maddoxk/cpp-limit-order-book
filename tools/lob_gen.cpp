// lob_gen: write a synthetic binary feed file.
//   lob_gen --out feed.bin [--seed 1] [--count 1000000]
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>

#include "lob/generator.hpp"
#include "lob/messages.hpp"

int main(int argc, char** argv) {
  std::string out_path;
  lob::GeneratorConfig cfg;
  std::size_t count = 1'000'000;

  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto value = [&]() -> const char* {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "missing value for %s\n", a.c_str());
        std::exit(2);
      }
      return argv[++i];
    };
    if (a == "--out") out_path = value();
    else if (a == "--seed") cfg.seed = std::strtoull(value(), nullptr, 10);
    else if (a == "--count") count = std::strtoull(value(), nullptr, 10);
    else {
      std::fprintf(stderr, "usage: lob_gen --out FILE [--seed N] [--count N]\n");
      return 2;
    }
  }
  if (out_path.empty()) {
    std::fprintf(stderr, "usage: lob_gen --out FILE [--seed N] [--count N]\n");
    return 2;
  }

  lob::FeedGenerator gen(cfg);
  const auto messages = gen.generate(count);
  const auto bytes = lob::encode_feed(messages);

  std::ofstream f(out_path, std::ios::binary);
  f.write(reinterpret_cast<const char*>(bytes.data()),
          static_cast<std::streamsize>(bytes.size()));
  if (!f) {
    std::fprintf(stderr, "failed to write %s\n", out_path.c_str());
    return 1;
  }
  std::printf("wrote %zu messages (%zu bytes) to %s, seed=%llu\n", messages.size(),
              bytes.size(), out_path.c_str(),
              static_cast<unsigned long long>(cfg.seed));
  return 0;
}
