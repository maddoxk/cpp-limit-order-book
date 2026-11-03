add_executable(lob_bench bench_book.cpp bench_replay.cpp)
target_link_libraries(lob_bench PRIVATE lob benchmark::benchmark benchmark::benchmark_main)
