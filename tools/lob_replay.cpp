// lob_replay: drive an order book from a feed file and print summary stats.
//   lob_replay feed.bin [--threaded] [--queue 16384]
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "lob/messages.hpp"
#include "lob/replay.hpp"

namespace {
void print_opt(const char* label, const std::optional<lob::Price>& p) {
  if (p) std::printf("  %-18s %d\n", label, *p);
  else std::printf("  %-18s none\n", label);
}
}  // namespace

int main(int argc, char** argv) {
  std::string path;
  bool threaded = false;
  std::size_t queue = 1 << 14;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--threaded") threaded = true;
    else if (a == "--queue" && i + 1 < argc) queue = std::strtoull(argv[++i], nullptr, 10);
    else if (!a.empty() && a[0] != '-' && path.empty()) path = a;
    else {
      std::fprintf(stderr, "usage: lob_replay FILE [--threaded] [--queue N]\n");
      return 2;
    }
  }
  if (path.empty()) {
    std::fprintf(stderr, "usage: lob_replay FILE [--threaded] [--queue N]\n");
    return 2;
  }

  std::ifstream f(path, std::ios::binary);
  if (!f) {
    std::fprintf(stderr, "cannot open %s\n", path.c_str());
    return 1;
  }
  const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(f)),
                                        std::istreambuf_iterator<char>());

  const auto t0 = std::chrono::steady_clock::now();
  const lob::ReplayResult r = threaded ? lob::replay_bytes_threaded(bytes, queue)
                                       : lob::replay_bytes(bytes);
  const auto t1 = std::chrono::steady_clock::now();
  const double secs = std::chrono::duration<double>(t1 - t0).count();

  if (!r.header_ok) {
    std::fprintf(stderr, "%s is not a LOBF v%u feed file\n", path.c_str(), lob::kFileVersion);
    return 1;
  }
  const lob::ReplayStats& s = r.stats;
  std::printf("mode: %s\n", threaded ? "two threads (decoder -> SPSC queue -> book)"
                                     : "single thread");
  std::printf("  messages           %llu\n", static_cast<unsigned long long>(s.messages));
  std::printf("  limit orders       %llu\n", static_cast<unsigned long long>(s.limit_orders));
  std::printf("  market orders      %llu\n", static_cast<unsigned long long>(s.market_orders));
  std::printf("  cancels            %llu\n", static_cast<unsigned long long>(s.cancels));
  std::printf("  modifies           %llu\n", static_cast<unsigned long long>(s.modifies));
  std::printf("  executes           %llu\n", static_cast<unsigned long long>(s.executes));
  std::printf("  rejected           %llu\n", static_cast<unsigned long long>(s.rejects));
  std::printf("  trades             %llu\n", static_cast<unsigned long long>(s.trades));
  std::printf("  traded qty         %llu\n", static_cast<unsigned long long>(s.traded_qty));
  std::printf("  trade checksum     %016llx\n", static_cast<unsigned long long>(s.trade_checksum));
  print_opt("best bid", s.best_bid);
  print_opt("best ask", s.best_ask);
  std::printf("  resting orders     %llu (%llu bid levels, %llu ask levels)\n",
              static_cast<unsigned long long>(s.resting_orders),
              static_cast<unsigned long long>(s.bid_levels),
              static_cast<unsigned long long>(s.ask_levels));
  std::printf("  elapsed            %.3f s (%.2f M msgs/s, wall clock incl. decode)\n", secs,
              secs > 0 ? static_cast<double>(s.messages) / secs / 1e6 : 0.0);
  if (r.decode_status != lob::DecodeStatus::Ok) {
    std::fprintf(stderr, "decode stopped early: %s\n", lob::to_string(r.decode_status));
    return 1;
  }
  return 0;
}
