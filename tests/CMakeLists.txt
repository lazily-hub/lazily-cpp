# The top-level CMakeLists defaults CMAKE_BUILD_TYPE to Release, and CMake's
# stock CMAKE_CXX_FLAGS_<CONFIG> for Release/MinSizeRel/RelWithDebInfo carry
# -DNDEBUG, which compiles out every assert(). The suite is assert-driven, so
# under the default configuration the tests ran to completion while verifying
# nothing. Strip NDEBUG from the optimized configurations *for this directory
# only* -- the library and FFI targets keep their Release flags untouched, and
# tests stay optimized (-O3/-O2) so timing-sensitive cases behave as shipped.
foreach(_cfg RELEASE MINSIZEREL RELWITHDEBINFO)
  string(REGEX REPLACE "[-/]D *NDEBUG" "" CMAKE_CXX_FLAGS_${_cfg}
         "${CMAKE_CXX_FLAGS_${_cfg}}")
endforeach()
# Belt-and-braces for any toolchain/config that injects NDEBUG outside the
# per-config flag variables.
add_compile_options($<$<OR:$<CXX_COMPILER_ID:GNU>,$<CXX_COMPILER_ID:Clang>,$<CXX_COMPILER_ID:AppleClang>>:-UNDEBUG>)

add_executable(test_smoke test_smoke.cpp)
target_link_libraries(test_smoke PRIVATE lazily)
add_test(NAME smoke COMMAND test_smoke)

add_executable(test_core test_core.cpp)
target_link_libraries(test_core PRIVATE lazily)
add_test(NAME core COMMAND test_core)

add_executable(test_merge test_merge.cpp)
target_link_libraries(test_merge PRIVATE lazily)
add_test(NAME merge COMMAND test_merge)

# The Cell kernel (`#lzcellkernel`): SourceCell/FormulaCell over Cell<T, K>.
add_executable(test_cell_kernel test_cell_kernel.cpp)
target_link_libraries(test_cell_kernel PRIVATE lazily)
add_test(NAME cell_kernel COMMAND test_cell_kernel)

add_executable(test_computed_ripple_when test_computed_ripple_when.cpp)
target_link_libraries(test_computed_ripple_when PRIVATE lazily)
add_test(NAME computed_ripple_when COMMAND test_computed_ripple_when)

# Compile-fail proof: `formula.set(...)` must NOT compile. Built on demand and
# asserted to FAIL — a successful build fails the test (write protection lost).
add_executable(compile_fail_formula_set EXCLUDE_FROM_ALL compile_fail_formula_set.cpp)
target_link_libraries(compile_fail_formula_set PRIVATE lazily)
add_test(NAME formula_set_is_a_compile_error
  COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR}
          --target compile_fail_formula_set --config $<CONFIG>)
set_tests_properties(formula_set_is_a_compile_error PROPERTIES WILL_FAIL TRUE)

add_executable(test_relay test_relay.cpp)
target_link_libraries(test_relay PRIVATE lazily)
add_test(NAME relay COMMAND test_relay)

add_executable(test_statechart test_statechart.cpp)
target_link_libraries(test_statechart PRIVATE lazily)
add_test(NAME statechart COMMAND test_statechart)

add_executable(test_collections test_collections.cpp)
target_link_libraries(test_collections PRIVATE lazily)
add_test(NAME collections COMMAND test_collections)

add_executable(test_crdt test_crdt.cpp)
target_link_libraries(test_crdt PRIVATE lazily)
add_test(NAME crdt COMMAND test_crdt)

add_executable(test_crdt_tree test_crdt_tree.cpp)
target_link_libraries(test_crdt_tree PRIVATE lazily)
add_test(NAME CrdtTreeConformance COMMAND test_crdt_tree)

add_executable(test_ipc test_ipc.cpp)
target_link_libraries(test_ipc PRIVATE lazily lazily_ffi)
add_test(NAME ipc COMMAND test_ipc)

add_executable(test_queue test_queue.cpp)
target_link_libraries(test_queue PRIVATE lazily)
add_test(NAME queue COMMAND test_queue)

add_executable(test_work_queue test_work_queue.cpp)
target_link_libraries(test_work_queue PRIVATE lazily)
add_test(NAME work_queue COMMAND test_work_queue)

add_executable(test_concurrency test_concurrency.cpp)
target_link_libraries(test_concurrency PRIVATE lazily)
add_test(NAME concurrency COMMAND test_concurrency)

add_executable(test_codec test_codec.cpp)
target_link_libraries(test_codec PRIVATE lazily)
add_test(NAME codec COMMAND test_codec)

add_executable(test_transport test_transport.cpp)
target_link_libraries(test_transport PRIVATE lazily)
add_test(NAME transport COMMAND test_transport)

add_executable(test_reactive_family test_reactive_family.cpp)
target_link_libraries(test_reactive_family PRIVATE lazily)
add_test(NAME reactive_family COMMAND test_reactive_family)

add_executable(test_thread_safe_reactive_family test_thread_safe_reactive_family.cpp)
target_link_libraries(test_thread_safe_reactive_family PRIVATE lazily)
add_test(NAME thread_safe_reactive_family COMMAND test_thread_safe_reactive_family)

add_executable(test_async_reactive_family test_async_reactive_family.cpp)
target_link_libraries(test_async_reactive_family PRIVATE lazily)
add_test(NAME async_reactive_family COMMAND test_async_reactive_family)

add_executable(test_family_sync test_family_sync.cpp)
target_link_libraries(test_family_sync PRIVATE lazily)
add_test(NAME family_sync COMMAND test_family_sync)

add_executable(test_reliable_sync test_reliable_sync.cpp)
target_link_libraries(test_reliable_sync PRIVATE lazily)
add_test(NAME reliable_sync COMMAND test_reliable_sync)

add_executable(test_outbox_store test_outbox_store.cpp)
target_link_libraries(test_outbox_store PRIVATE lazily)
add_test(NAME OutboxStoreConformance COMMAND test_outbox_store)

# Realtime + distributed primitive families (#lztime / #lzrateshape / #lzmemb /
# #lzcoord / #lzpresence / #lzwindow / #lzresilience / #lzservice). Each replays
# the canonical lazily-spec conformance fixtures read from the sibling checkout
# at ../lazily-spec/conformance/<family>/ — see the LAZILY_SPEC_CONFORMANCE_DIR
# block below (#lzspecconf).
add_executable(test_temporal_conformance test_temporal_conformance.cpp)
target_link_libraries(test_temporal_conformance PRIVATE lazily)
add_test(NAME TemporalConformance COMMAND test_temporal_conformance)

add_executable(test_rateshape_conformance test_rateshape_conformance.cpp)
target_link_libraries(test_rateshape_conformance PRIVATE lazily)
add_test(NAME RateshapeConformance COMMAND test_rateshape_conformance)

add_executable(test_membership_conformance test_membership_conformance.cpp)
target_link_libraries(test_membership_conformance PRIVATE lazily)
add_test(NAME MembershipConformance COMMAND test_membership_conformance)

add_executable(test_coordination_conformance test_coordination_conformance.cpp)
target_link_libraries(test_coordination_conformance PRIVATE lazily)
add_test(NAME CoordinationConformance COMMAND test_coordination_conformance)

add_executable(test_presence_conformance test_presence_conformance.cpp)
target_link_libraries(test_presence_conformance PRIVATE lazily)
add_test(NAME PresenceConformance COMMAND test_presence_conformance)

add_executable(test_windowing_conformance test_windowing_conformance.cpp)
target_link_libraries(test_windowing_conformance PRIVATE lazily)
add_test(NAME WindowingConformance COMMAND test_windowing_conformance)

add_executable(test_resilience_conformance test_resilience_conformance.cpp)
target_link_libraries(test_resilience_conformance PRIVATE lazily)
add_test(NAME ResilienceConformance COMMAND test_resilience_conformance)

add_executable(test_service_conformance test_service_conformance.cpp)
target_link_libraries(test_service_conformance PRIVATE lazily)
add_test(NAME ServiceConformance COMMAND test_service_conformance)

add_executable(test_seqcrdt_conformance test_seqcrdt_conformance.cpp)
target_link_libraries(test_seqcrdt_conformance PRIVATE lazily)
add_test(NAME SeqCrdtConformance COMMAND test_seqcrdt_conformance)

add_executable(test_topic_conformance test_topic_conformance.cpp)
target_link_libraries(test_topic_conformance PRIVATE lazily)
add_test(NAME TopicConformance COMMAND test_topic_conformance)

# Reactive-graph disposal/teardown + invalidation corpus (#lzspecedgeindex).
# Replays ../lazily-spec/conformance/reactive-graph/. lazily-cpp replayed NONE
# of this corpus until now — only lazily-rs did, family-wide, which is why an
# invalidation-cascade defect shipped undetected in two other bindings.
add_executable(test_reactive_graph_conformance test_reactive_graph_conformance.cpp)
target_link_libraries(test_reactive_graph_conformance PRIVATE lazily)
add_test(NAME ReactiveGraphConformance COMMAND test_reactive_graph_conformance)

# Canonical conformance fixtures (#lzspecconf).
#
# These suites read fixtures from the sibling lazily-spec checkout, NOT from a
# copy inside this repo. A vendored copy makes drift invisible: the suite goes
# green against whatever this repo happens to hold, which is exactly how a
# fixture elsewhere in the family shrank to a third of its source unnoticed.
# lazily-cpp vendored its fixtures under tests/conformance/ and its CI never
# cloned the spec, so it had never once validated against the canonical corpus.
#
# The path is baked in at configure time so it resolves identically regardless
# of the working directory ctest launches the binary from (`ctest --test-dir
# build` runs from the build tree, not the source root). CMAKE_CURRENT_SOURCE_DIR
# is <repo>/tests, so ../../lazily-spec is the sibling checkout — the same
# relative arrangement LAZILY_FORMAL_DIR above relies on. Nothing is absolute in
# the source; CMake derives it from the source tree location.
#
# Absence is a SKIP (CTest exit 77) with an explicit message rather than a
# silent pass. CI asserts the directory exists so a skip cannot go unnoticed
# there, and each suite additionally asserts a positive distinct-fixture count.
set(LAZILY_SPEC_CONFORMANCE_TESTS
  test_crdt_tree
  test_outbox_store
  test_temporal_conformance
  test_rateshape_conformance
  test_membership_conformance
  test_coordination_conformance
  test_presence_conformance
  test_windowing_conformance
  test_resilience_conformance
  test_service_conformance
  test_seqcrdt_conformance
  test_topic_conformance
  test_reactive_graph_conformance)

foreach(_target IN LISTS LAZILY_SPEC_CONFORMANCE_TESTS)
  target_compile_definitions(${_target} PRIVATE
    LAZILY_SPEC_CONFORMANCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../../lazily-spec/conformance")
endforeach()

set_tests_properties(
  CrdtTreeConformance
  OutboxStoreConformance
  TemporalConformance
  RateshapeConformance
  MembershipConformance
  CoordinationConformance
  PresenceConformance
  WindowingConformance
  ResilienceConformance
  ServiceConformance
  ReactiveGraphConformance
  PROPERTIES SKIP_RETURN_CODE 77)

# Gates the suite on the sibling lazily-formal Lean model compiling (`lake
# build`). Skips gracefully (CTest exit code 77) when the sibling checkout or
# the Lean toolchain is absent. The submodule sits next to this repo
# (src/lazily-cpp <-> src/lazily-formal); bake that path in for the runtime
# resolver, overridable at run time via LAZILY_FORMAL_PATH.
add_executable(test_formal test_formal.cpp)
target_compile_definitions(test_formal PRIVATE
  LAZILY_FORMAL_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../../lazily-formal")
add_test(NAME formal COMMAND test_formal)
set_tests_properties(formal PROPERTIES SKIP_RETURN_CODE 77)

# Optional, heavier TSan harness (not in the default suite). Build & run with:
#   cmake -S . -B build -DLAZILY_BUILD_TSAN_TEST=ON \
#     -DCMAKE_CXX_FLAGS="-fsanitize=thread -g" -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=thread"
#   ctest --test-dir build -R tsan --output-on-failure
option(LAZILY_BUILD_TSAN_TEST "Build the ThreadSanitizer concurrency stress harness" OFF)
if(LAZILY_BUILD_TSAN_TEST)
  add_executable(test_tsan_stress tsan_stress.cpp)
  target_link_libraries(test_tsan_stress PRIVATE lazily)
  add_test(NAME tsan_stress COMMAND test_tsan_stress)
endif()
