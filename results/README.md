# Benchmark results

`ci_bench_results.json` is the unmodified Google Benchmark JSON uploaded as the
`benchmark-results` artifact by the `Benchmarks` job of GitHub Actions run
41664995641 (PR #10). Command used by CI:

```
./build/bench/lob_bench --benchmark_repetitions=5 --benchmark_report_aggregates_only=true \
    --benchmark_out=bench_results.json --benchmark_out_format=json
```

Environment: GitHub Actions `ubuntu-latest` (shared, noisy), 2 vCPU, AMD EPYC 7763,
g++ 15.2, CMake Release build. Numbers will differ on other machines and between runs.
Re-run the job to refresh; the README table is copied from this file's `_mean` rows.
