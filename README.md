# cpp-limit-order-book

A single-symbol **limit order book and matching engine in C++20**, with an ITCH-style
binary feed, a seeded synthetic feed generator, a replay tool, and a lock-free
single-producer/single-consumer (SPSC) queue connecting a decoder thread to the book thread.

It is a learning/portfolio project, written to be read: every data-structure choice is
explained in [docs/DESIGN.md](docs/DESIGN.md). It is **not** a production exchange and
uses **no real market data**.

## What it does

- Price-time priority matching: limit and market orders, partial fills, multi-level sweeps.
- `add` / `cancel` / `modify` (reducing quantity keeps queue priority; a price change or a
  size increase loses it) / `execute` (feed-reported fill). Trades print at the resting order's price.
- Best bid/ask and L2 depth snapshots. Integer tick prices, no floating point in the book.
- Cache-friendly layout: pooled orders linked into per-price FIFO queues by index, flat sorted
  vector of price levels, hash map from order id to slot.
- Binary feed codec (4 message types, little-endian, bounds-checked decode), seeded generator, replay tool.
- `SpscQueue<T>`: bounded lock-free ring buffer; the two-thread replay pipeline is tested under ThreadSanitizer.

## Build and run

Requires CMake >= 3.20 and a C++20 compiler (developed against what GitHub Actions
`ubuntu-latest` provides: g++ and clang++). GoogleTest and Google Benchmark are downloaded by CMake.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure

# generate a synthetic feed and replay it
./build/lob_gen --out feed.bin --seed 42 --count 1000000
./build/lob_replay feed.bin                 # single thread
./build/lob_replay feed.bin --threaded      # decoder thread -> SPSC queue -> book thread

# benchmarks
cmake -S . -B build-bench -DCMAKE_BUILD_TYPE=Release -DLOB_BUILD_BENCH=ON
cmake --build build-bench -j && ./build-bench/bench/lob_bench

# sanitizers
cmake -S . -B build-asan -DLOB_SANITIZE=address,undefined   # or -DLOB_SANITIZE=thread
```

## Testing

GoogleTest suite covering matching, sweeps, time priority, modify/cancel/execute edge cases,
validation, the codec (round-trip, truncation at every length, bad bytes), the SPSC queue
(including a two-thread test), replay determinism, and a threaded-equals-single-threaded check.
A **randomized property test** drives the book and an independent `std::map`/`std::deque`
reference implementation with the same random operations and compares trades, top of book and
full depth after every step. CI runs g++ and clang++ in Release and Debug, an ASan+UBSan job,
and a ThreadSanitizer job.

## Benchmark results

Measured by the `Benchmarks` CI job (run 41664995641): **GitHub Actions ubuntu-latest (shared, noisy)**,
2 vCPU AMD EPYC 7763, g++ 15.2, Release, mean of 5 repetitions. Raw JSON and the command are in
[results/](results/). Treat these as rough orders of magnitude, not as a claim about any other hardware.

| Benchmark | What is measured | Mean throughput | Run-to-run stddev |
|---|---|---|---|
| `AddPassive` | 20k non-crossing limit adds across 100 levels per side (includes building the book) | 15.2 M orders/s | 0.79 M |
| `CancelRandomOrder` | cancel 20k resting orders in random order | 8.5 M cancels/s | 0.01 M |
| `MatchMarketOrders` | 20k single-share market buys, one trade each | 26.6 M orders/s | 0.09 M |
| `SweepWholeBook` | one market order consuming 20k resting orders | 15.1 M fills/s | 0.04 M |
| `ApplyPreDecoded` | 500k synthetic feed messages applied to a book | 12.0 M msgs/s | 0.9 M |
| `ReplayDecodeAndApply1Thread` | decode bytes + apply, one thread | 11.3 M msgs/s | 1.3 M |
| `ReplayTwoThreadsSpsc` | decoder thread -> SPSC queue -> book thread (wall clock) | 9.6 M msgs/s | 0.03 M |

Observation: the two-thread pipeline was *slower* than doing everything on one thread on this
2-vCPU shared runner. Decoding a fixed-size message is very cheap compared with the cost of
handing it between cores, so the queue adds overhead rather than hiding latency. I have not
investigated further (e.g. batching) and make no claim beyond what the table shows.

Per-operation costs were not measured individually (no latency histograms); throughput numbers
are averages over a batch.

## Limitations

- **Synthetic data only.** The generator is a simple statistical model (random-walk mid price,
  exponential offsets and sizes). The books it produces stay shallow (about 160 resting orders
  after 200k messages), unlike real markets. Benchmark numbers depend on this and say nothing about real feeds.
- Single symbol, single thread owns the book; only limit and market orders. No stop, iceberg,
  IOC/FOK, auctions, self-trade prevention, risk checks, or market-data gateways.
- Market-order remainders are discarded; modify with quantity 0 is rejected (use cancel).
- The id index is `std::unordered_map` (node based), and inserting/erasing a price level in the
  middle of the sorted vector is O(levels). Both are discussed in DESIGN.md as deliberate trade-offs.
- Quantities are 32-bit, prices 32-bit ticks; no sequence numbers, checksums or gap handling on the wire format.
- The "ITCH-style" format is inspired by Nasdaq ITCH but is **not** compatible with it.
- The SPSC pipeline busy-waits with `yield()`; it was tested for correctness under TSan, not tuned.
- Developed without a local C++ compiler: everything is built and tested in GitHub Actions only.

## Layout

```
include/lob/   public headers (types, order_book, messages, generator, replay, spsc_queue)
src/           implementations
tools/         lob_gen, lob_replay
tests/         GoogleTest suite + reference book
bench/         Google Benchmark suites
docs/DESIGN.md architecture and trade-offs
results/       CI benchmark JSON and how it was produced
```

License: MIT.
