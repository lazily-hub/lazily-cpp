add_executable(lazily_bench bench_main.cpp)
target_link_libraries(lazily_bench PRIVATE lazily)

# wasm32 (`#lzcppwasm`). The harness measures ThreadSafeContext contention and
# read scaling, so it builds in the THREADED tier — recorded in the BENCHMARKS.md
# environment line, because a single-threaded run would report those rows as
# something they are not. NODERAWFS/EXIT_RUNTIME match the conformance runners;
# -fexceptions is required for the same reason (emcc defaults to ignoring them).
if(EMSCRIPTEN)
  target_compile_options(lazily_bench PRIVATE -fexceptions -pthread)
  target_link_options(lazily_bench PRIVATE
    -sNODERAWFS=1 -sEXIT_RUNTIME=1 -sALLOW_MEMORY_GROWTH=1 -sSTACK_SIZE=4MB
    -sMAXIMUM_MEMORY=4GB -fexceptions -pthread -sPTHREAD_POOL_SIZE=8)
endif()

# Width-ladder pub/sub load test (#lzspecedgeindex). Manual and on-demand — it
# is not registered with ctest and `make check` never runs it, because the top
# rungs allocate tens of GiB and take minutes. Build and run it explicitly:
#   cmake --build build --target lazily_pubsub_load
#   ./build/benches/lazily_pubsub_load 10485760
add_executable(lazily_pubsub_load EXCLUDE_FROM_ALL pubsub_load.cpp)
target_link_libraries(lazily_pubsub_load PRIVATE lazily)
