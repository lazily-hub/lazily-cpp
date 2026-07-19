add_executable(lazily_bench bench_main.cpp)
target_link_libraries(lazily_bench PRIVATE lazily)

# Width-ladder pub/sub load test (#lzspecedgeindex). Manual and on-demand — it
# is not registered with ctest and `make check` never runs it, because the top
# rungs allocate tens of GiB and take minutes. Build and run it explicitly:
#   cmake --build build --target lazily_pubsub_load
#   ./build/benches/lazily_pubsub_load 10485760
add_executable(lazily_pubsub_load EXCLUDE_FROM_ALL pubsub_load.cpp)
target_link_libraries(lazily_pubsub_load PRIVATE lazily)
