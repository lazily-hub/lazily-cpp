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

# Emscripten/wasm32 builds use their own tier-driven registry and stop here
# (`#lzcppwasm`). Everything below this point is the native suite and is left
# exactly as it was — a wasm build cannot regress it, and the wasm registry
# cannot silently inherit a native-only target.
if(EMSCRIPTEN)
  include(${CMAKE_CURRENT_SOURCE_DIR}/wasm.cmake)
  return()
endif()

# ── Canonical conformance registration (#lzcorpusabsencehandling) ──────────
#
# The suites below read fixtures from the sibling lazily-spec checkout rather
# than from a copy inside this repo; the rationale, the path derivation, and the
# absence-is-a-SKIP contract are documented at the LAZILY_SPEC_CONFORMANCE_DIR
# block further down, which this helper is the registration half of.
#
# Registration is ONE call, deliberately. Three things must be true of every
# suite that reads the checkout — the corpus path is baked in, the test exists,
# and ctest reads its exit-77 as SKIP — and they used to live in three separate
# places: a target list, an `add_test()`, and a hand-maintained list of ctest
# NAMES passed to `set_tests_properties(... SKIP_RETURN_CODE 77)`. The names
# list drifted from the target list three separate times (four targets, then
# three, then six), and each drift turned "no sibling checkout" from a skip into
# a hard ctest FAILURE for those suites. The first two were repaired by editing
# the list again, which is precisely what left a third instance possible. So the
# list is gone: the three properties are inseparable now because there is one
# place to state them. The guard at the bottom of this file closes the remaining
# hole — a corpus reader registered with a bare `add_test()`.
set(LAZILY_SPEC_CONFORMANCE_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/../../lazily-spec/conformance")

function(lazily_add_spec_conformance_test _ctest_name _target)
  target_compile_definitions(${_target} PRIVATE
    LAZILY_SPEC_CONFORMANCE_DIR="${LAZILY_SPEC_CONFORMANCE_DIR}")
  add_test(NAME ${_ctest_name} COMMAND ${_target})
  # `require_spec_checkout_or_skip()` (tests/test_spec_fixture.hpp) exits 77 to
  # mean SKIP. Without SKIP_RETURN_CODE, ctest reads that 77 as a FAILURE and an
  # absent sibling checkout looks like a broken build instead of a skipped suite.
  set_tests_properties(${_ctest_name} PROPERTIES SKIP_RETURN_CODE 77)
  set_property(GLOBAL APPEND PROPERTY LAZILY_SPEC_CONFORMANCE_TARGETS ${_target})
endfunction()

add_executable(test_smoke test_smoke.cpp)
target_link_libraries(test_smoke PRIVATE lazily)
add_test(NAME smoke COMMAND test_smoke)

add_executable(test_stdlib_conformance test_stdlib_conformance.cpp)
target_link_libraries(test_stdlib_conformance PRIVATE lazily)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(test_stdlib_conformance PRIVATE
    -fsanitize=address,undefined
    -fno-omit-frame-pointer)
  target_link_options(test_stdlib_conformance PRIVATE
    -fsanitize=address,undefined)
endif()
lazily_add_spec_conformance_test(StdlibConformance test_stdlib_conformance)

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

# The fortified compute view (`#lzcellkernel`): value-threaded tracking, the
# untracked escape, effect tracking, the nesting barrier, and the non-copyable
# static_asserts. Mirrors lazily-rs tests/compute_fortification.rs.
add_executable(compute_fortification compute_fortification.cpp)
target_link_libraries(compute_fortification PRIVATE lazily)
add_test(NAME compute_fortification COMMAND compute_fortification)

# Compile-fail proof: a `Compute` view must NOT be copyable (non-escapable).
# WILL_FAIL — a successful build fails the test (fortification lost).
add_executable(compile_fail_compute_escape EXCLUDE_FROM_ALL compile_fail_compute_escape.cpp)
target_link_libraries(compile_fail_compute_escape PRIVATE lazily)
add_test(NAME compute_escape_is_a_compile_error
  COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR}
          --target compile_fail_compute_escape --config $<CONFIG>)
set_tests_properties(compute_escape_is_a_compile_error PROPERTIES WILL_FAIL TRUE)

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

# Replays ../lazily-spec/conformance/statechart/ — seven canonical fixtures the
# binding had no runner for at all (test_statechart.cpp builds its charts by hand
# and never opens the corpus).
add_executable(test_statechart_conformance test_statechart_conformance.cpp)
target_link_libraries(test_statechart_conformance PRIVATE lazily)
lazily_add_spec_conformance_test(StatechartConformance test_statechart_conformance)

add_executable(test_collections test_collections.cpp)
target_link_libraries(test_collections PRIVATE lazily)
add_test(NAME collections COMMAND test_collections)

# Replays the remaining canonical collections corpus: MergeCell algebra,
# keyed reconciliation, SemTree, stable IDs, and TextCrdt convergence/deltas.
add_executable(test_collections_fixture_conformance
  test_collections_fixture_conformance.cpp)
target_link_libraries(test_collections_fixture_conformance PRIVATE lazily)
lazily_add_spec_conformance_test(CollectionsFixtureConformance test_collections_fixture_conformance)

# Replays ../lazily-spec/conformance/receipts/ — one canonical fixture the
# binding had no runner for in any form.
add_executable(test_receipts_conformance test_receipts_conformance.cpp)
target_link_libraries(test_receipts_conformance PRIVATE lazily)
lazily_add_spec_conformance_test(ReceiptsConformance test_receipts_conformance)

# Replays ../lazily-spec/conformance/distributed/anti_entropy_converge.json —
# CRDT-plane convergence, applied-op accounting, idempotence, order independence.
add_executable(test_distributed_conformance test_distributed_conformance.cpp)
target_link_libraries(test_distributed_conformance PRIVATE lazily)
lazily_add_spec_conformance_test(DistributedConformance test_distributed_conformance)

# Replays ../lazily-spec/conformance/codec/frame_roundtrip_json.json THROUGH the
# json reference codec (#lzcppjsoncodec). Every other conformance binary replays
# fixture CONTENT; this one replays a codec, which is why it is the only rung
# that could have caught lazily-cpp shipping codec.hpp while speaking no JSON.
add_executable(test_codec_conformance test_codec_conformance.cpp)
target_link_libraries(test_codec_conformance PRIVATE lazily)
lazily_add_spec_conformance_test(CodecConformance test_codec_conformance)

# Replays capability_handshake.json through the production negotiation API,
# retaining the common ceiling and mutual fragmentation state.
add_executable(test_capability_conformance test_capability_conformance.cpp)
target_link_libraries(test_capability_conformance PRIVATE lazily)
lazily_add_spec_conformance_test(CapabilityConformance test_capability_conformance)

# Replays ../lazily-spec/conformance/codec/nodeid_exact_range.json through BOTH
# codecs (#lzspecdecoderbound). The clause it carries — a decoder that cannot
# represent a received identifier exactly MUST reject the frame rather than
# round it — is the half protocol.md left undefined, and lazily-cpp's int64_t
# NodeId is narrower than the u64 wire type, so refusal is its conforming
# answer for the top of the range. The same binary pins the error TYPE of that
# refusal: it used to escape as std::out_of_range, outside the std::runtime_error
# every caller guards a decode with.
add_executable(test_nodeid_exact_range_conformance test_nodeid_exact_range_conformance.cpp)
target_link_libraries(test_nodeid_exact_range_conformance PRIVATE lazily)
lazily_add_spec_conformance_test(NodeIdExactRangeConformance test_nodeid_exact_range_conformance)

# Replays ../lazily-spec/conformance/codec/nodekey_null_leniency.json through
# BOTH codecs (#lzkeynullstrict). Omit-when-absent binds the ENCODER; a decoder
# must read an omitted `key` and an explicit `key: null` alike as absent, and
# must still emit the omitted form. The re-encode half is why this is a separate
# binary rather than an extra scenario on the round-trip runner: it inspects the
# produced frame's FIELD SET, which no assertion over a decoded value can reach.
add_executable(test_nodekey_null_leniency_conformance test_nodekey_null_leniency_conformance.cpp)
target_link_libraries(test_nodekey_null_leniency_conformance PRIVATE lazily)
lazily_add_spec_conformance_test(NodeKeyNullLeniencyConformance test_nodekey_null_leniency_conformance)

# Replays ../lazily-spec/conformance/codec/blob_backend_discriminator.json
# through BOTH codecs (#lzblobbackendstrict). The clause splits the two forms a
# lenient decoder conflated: an OMITTED `backend` is the forward-compat channel
# and MUST decode as `shm`, while a PRESENT token outside the enum MUST be
# refused naming the token. lazily-cpp normalized the unknown token to `Shm` and
# documented it as forward compatibility; that reads a non-shm descriptor into
# the shm table, which is the misroute `resolve_wrong_backend` excludes
# structurally. Like the NodeKey runner this is a separate binary because half
# its assertions inspect the RE-ENCODED frame's field set, which no assertion
# over a decoded value can reach.
add_executable(test_blob_backend_discriminator_conformance
  test_blob_backend_discriminator_conformance.cpp)
target_link_libraries(test_blob_backend_discriminator_conformance PRIVATE lazily)
lazily_add_spec_conformance_test(BlobBackendDiscriminatorConformance test_blob_backend_discriminator_conformance)

add_executable(test_crdt test_crdt.cpp)
target_link_libraries(test_crdt PRIVATE lazily)
add_test(NAME crdt COMMAND test_crdt)

add_executable(test_crdt_tree test_crdt_tree.cpp)
target_link_libraries(test_crdt_tree PRIVATE lazily)
lazily_add_spec_conformance_test(CrdtTreeConformance test_crdt_tree)

add_executable(test_ipc test_ipc.cpp)
target_link_libraries(test_ipc PRIVATE lazily lazily_ffi)
lazily_add_spec_conformance_test(ipc test_ipc)

add_executable(test_queue test_queue.cpp)
target_link_libraries(test_queue PRIVATE lazily)
add_test(NAME queue COMMAND test_queue)

add_executable(test_work_queue test_work_queue.cpp)
target_link_libraries(test_work_queue PRIVATE lazily)
add_test(NAME work_queue COMMAND test_work_queue)

add_executable(test_work_queue_conformance test_work_queue_conformance.cpp)
target_link_libraries(test_work_queue_conformance PRIVATE lazily)
lazily_add_spec_conformance_test(WorkQueueConformance test_work_queue_conformance)

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

# Replays the canonical materialization fixture bytes (the hand-mirrored copies
# in test_reactive_family cannot detect drift). Reads the sibling lazily-spec
# checkout, so it registers through lazily_add_spec_conformance_test() and skips
# (77) when that checkout is absent.
add_executable(test_materialization_conformance test_materialization_conformance.cpp)
target_link_libraries(test_materialization_conformance PRIVATE lazily)
lazily_add_spec_conformance_test(MaterializationConformance test_materialization_conformance)

# The thread-safe / async shells of the same two fixtures. Separate binary
# because ThreadSafeContext and AsyncContext force the -pthread wasm tier while
# the core-tier materialization runner above must stay single-threaded
# (`#lzcppmatreplay`); the replay itself is shared via
# test_materialization_replay.hpp.
add_executable(test_materialization_family_conformance test_materialization_family_conformance.cpp)
target_link_libraries(test_materialization_family_conformance PRIVATE lazily)
lazily_add_spec_conformance_test(MaterializationFamilyConformance test_materialization_family_conformance)

add_executable(test_collections_family_conformance test_collections_family_conformance.cpp)
target_link_libraries(test_collections_family_conformance PRIVATE lazily)
lazily_add_spec_conformance_test(CollectionsFamilyConformance test_collections_family_conformance)

add_executable(test_signaling_conformance test_signaling_conformance.cpp)
target_link_libraries(test_signaling_conformance PRIVATE lazily)
lazily_add_spec_conformance_test(SignalingConformance test_signaling_conformance)

add_executable(test_queue_conformance test_queue_conformance.cpp)
target_link_libraries(test_queue_conformance PRIVATE lazily)
lazily_add_spec_conformance_test(QueueConformance test_queue_conformance)

add_executable(test_queue_family_conformance test_queue_family_conformance.cpp)
target_link_libraries(test_queue_family_conformance PRIVATE lazily)
lazily_add_spec_conformance_test(QueueFamilyConformance test_queue_family_conformance)

# Transport-agnostic reactive ingress (`#designimplementtransport`). The algebra
# invariants + shell reactivity live in test_ingress; the canonical corpus replay
# against all three flavours (with the enforced flavour ledger) lives in
# test_ingress_conformance.
add_executable(test_ingress test_ingress.cpp)
target_link_libraries(test_ingress PRIVATE lazily)
add_test(NAME ingress COMMAND test_ingress)

add_executable(test_ingress_conformance test_ingress_conformance.cpp)
target_link_libraries(test_ingress_conformance PRIVATE lazily)
lazily_add_spec_conformance_test(IngressConformance test_ingress_conformance)

add_executable(test_family_sync test_family_sync.cpp)
target_link_libraries(test_family_sync PRIVATE lazily)
lazily_add_spec_conformance_test(family_sync test_family_sync)

add_executable(test_reliable_sync test_reliable_sync.cpp)
target_link_libraries(test_reliable_sync PRIVATE lazily)
lazily_add_spec_conformance_test(reliable_sync test_reliable_sync)

add_executable(test_outbox_store test_outbox_store.cpp)
target_link_libraries(test_outbox_store PRIVATE lazily)
lazily_add_spec_conformance_test(OutboxStoreConformance test_outbox_store)

# Realtime + distributed primitive families (#lztime / #lzrateshape / #lzmemb /
# #lzcoord / #lzpresence / #lzwindow / #lzresilience / #lzservice). Each replays
# the canonical lazily-spec conformance fixtures read from the sibling checkout
# at ../lazily-spec/conformance/<family>/ — see the LAZILY_SPEC_CONFORMANCE_DIR
# block below (#lzspecconf).
add_executable(test_temporal_conformance test_temporal_conformance.cpp)
target_link_libraries(test_temporal_conformance PRIVATE lazily)
lazily_add_spec_conformance_test(TemporalConformance test_temporal_conformance)

add_executable(test_rateshape_conformance test_rateshape_conformance.cpp)
target_link_libraries(test_rateshape_conformance PRIVATE lazily)
lazily_add_spec_conformance_test(RateshapeConformance test_rateshape_conformance)

add_executable(test_membership_conformance test_membership_conformance.cpp)
target_link_libraries(test_membership_conformance PRIVATE lazily)
lazily_add_spec_conformance_test(MembershipConformance test_membership_conformance)

add_executable(test_coordination_conformance test_coordination_conformance.cpp)
target_link_libraries(test_coordination_conformance PRIVATE lazily)
lazily_add_spec_conformance_test(CoordinationConformance test_coordination_conformance)

add_executable(test_presence_conformance test_presence_conformance.cpp)
target_link_libraries(test_presence_conformance PRIVATE lazily)
lazily_add_spec_conformance_test(PresenceConformance test_presence_conformance)

add_executable(test_windowing_conformance test_windowing_conformance.cpp)
target_link_libraries(test_windowing_conformance PRIVATE lazily)
lazily_add_spec_conformance_test(WindowingConformance test_windowing_conformance)

add_executable(test_resilience_conformance test_resilience_conformance.cpp)
target_link_libraries(test_resilience_conformance PRIVATE lazily)
lazily_add_spec_conformance_test(ResilienceConformance test_resilience_conformance)

add_executable(test_service_conformance test_service_conformance.cpp)
target_link_libraries(test_service_conformance PRIVATE lazily)
lazily_add_spec_conformance_test(ServiceConformance test_service_conformance)

add_executable(test_seqcrdt_conformance test_seqcrdt_conformance.cpp)
target_link_libraries(test_seqcrdt_conformance PRIVATE lazily)
lazily_add_spec_conformance_test(SeqCrdtConformance test_seqcrdt_conformance)

add_executable(test_topic_conformance test_topic_conformance.cpp)
target_link_libraries(test_topic_conformance PRIVATE lazily)
lazily_add_spec_conformance_test(TopicConformance test_topic_conformance)

add_executable(test_lossless_tree_conformance test_lossless_tree_conformance.cpp)
target_link_libraries(test_lossless_tree_conformance PRIVATE lazily)
lazily_add_spec_conformance_test(LosslessTreeConformance test_lossless_tree_conformance)

# `diff` returns ops in canonical (counter, peer) order — a cross-binding
# contract, because the shared corpus addresses the returned ops POSITIONALLY
# (`deliver.only: [0, 2]` in non_contiguous_anti_entropy.json). Reversing or
# deleting the sort leaves the corpus green, so the order needs a direct test.
# Not a conformance suite: it reads no fixture and its include closure never
# reaches the skip-or-require seam, so it registers with a bare add_test() and
# the guard at the bottom of this file correctly leaves it alone.
# #lzdifforderallbindings
add_executable(test_lossless_tree_diff_order test_lossless_tree_diff_order.cpp)
target_link_libraries(test_lossless_tree_diff_order PRIVATE lazily)
add_test(NAME lossless_tree_diff_order COMMAND test_lossless_tree_diff_order)

add_executable(test_command_conformance test_command_conformance.cpp)
target_link_libraries(test_command_conformance PRIVATE lazily)
lazily_add_spec_conformance_test(CommandConformance test_command_conformance)

# Reactive-graph disposal/teardown + invalidation corpus (#lzspecedgeindex).
# Replays ../lazily-spec/conformance/reactive-graph/. lazily-cpp replayed NONE
# of this corpus until now — only lazily-rs did, family-wide, which is why an
# invalidation-cascade defect shipped undetected in two other bindings.
add_executable(test_reactive_graph_conformance test_reactive_graph_conformance.cpp)
target_link_libraries(test_reactive_graph_conformance PRIVATE lazily)
lazily_add_spec_conformance_test(ReactiveGraphConformance test_reactive_graph_conformance)

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
# Every suite above registers through lazily_add_spec_conformance_test(), which
# states all three of those properties in one call. What used to stand here were
# the two lists that stated them separately -- a list of TARGETS carrying the
# compile define, and a list of ctest NAMES carrying SKIP_RETURN_CODE 77 -- and
# the second drifted from the first three separate times. Each drift meant that
# for the missed suites an absent sibling checkout was reported as a hard ctest
# FAILURE rather than a skip, so a checkout without the sibling read as a broken
# build. Twice that was repaired by editing the names list again; the third time
# the lists were removed instead, which is why there is nothing to keep in sync
# below this point.
#
# The guard below covers the one case the helper cannot: a NEW corpus reader
# wired up with a bare add_test(). Membership is derived from the SOURCES rather
# than declared -- a target whose transitive local-include closure reaches
# `require_spec_checkout_or_skip` reads the corpus by construction and must
# therefore have been registered. Configure fails and names it when it was not.
#
# (tests/test_spec_fixture.hpp additionally #errors when LAZILY_SPEC_CONFORMANCE_DIR
# is undefined, so an unregistered reader already fails to BUILD. That covers the
# same hole one stage later and only for translation units that include the seam
# header; this one fails at configure time and says what to do about it.)
function(_lazily_target_reads_spec_corpus _target _out)
  get_target_property(_sources ${_target} SOURCES)
  if(NOT _sources)
    set(${_out} FALSE PARENT_SCOPE)
    return()
  endif()
  set(_pending "")
  foreach(_source IN LISTS _sources)
    get_filename_component(_abs "${_source}" ABSOLUTE
                           BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
    list(APPEND _pending "${_abs}")
  endforeach()

  set(_seen "")
  set(_reads FALSE)
  while(_pending)
    list(POP_FRONT _pending _file)
    if(NOT EXISTS "${_file}" OR "${_file}" IN_LIST _seen)
      continue()
    endif()
    list(APPEND _seen "${_file}")
    # Re-configure when a scanned source changes; a guard reading a stale
    # snapshot of the tree is a guard that stops guarding after the first edit.
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_file}")
    file(READ "${_file}" _text)
    if(_text MATCHES "require_spec_checkout_or_skip")
      set(_reads TRUE)
    endif()
    # Follow local includes only ("..."); a system include cannot reach the seam.
    string(REGEX MATCHALL "#[ \t]*include[ \t]*\"[^\"]+\"" _includes "${_text}")
    get_filename_component(_dir "${_file}" DIRECTORY)
    foreach(_include IN LISTS _includes)
      string(REGEX REPLACE "^#[ \t]*include[ \t]*\"([^\"]+)\"$" "\\1"
             _relative "${_include}")
      get_filename_component(_abs "${_dir}/${_relative}" ABSOLUTE)
      list(APPEND _pending "${_abs}")
    endforeach()
  endwhile()
  set(${_out} ${_reads} PARENT_SCOPE)
endfunction()

get_property(_lazily_registered GLOBAL PROPERTY LAZILY_SPEC_CONFORMANCE_TARGETS)
get_property(_lazily_dir_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
set(_lazily_unregistered "")
foreach(_target IN LISTS _lazily_dir_targets)
  get_target_property(_type ${_target} TYPE)
  if(NOT _type STREQUAL "EXECUTABLE")
    continue()
  endif()
  _lazily_target_reads_spec_corpus(${_target} _reads)
  if(_reads AND NOT "${_target}" IN_LIST _lazily_registered)
    list(APPEND _lazily_unregistered ${_target})
  endif()
endforeach()

if(_lazily_unregistered)
  string(REPLACE ";" "\n  " _lazily_unregistered_pretty "${_lazily_unregistered}")
  message(FATAL_ERROR
    "unregistered canonical-conformance runner(s):\n  "
    "${_lazily_unregistered_pretty}\n"
    "Each reads the sibling lazily-spec checkout through "
    "require_spec_checkout_or_skip(), which exits 77 to mean SKIP. Added with a "
    "bare add_test(), that 77 has no SKIP_RETURN_CODE behind it, so ctest "
    "reports a FAILURE and a checkout without the sibling looks like a broken "
    "build. Register with "
    "lazily_add_spec_conformance_test(<ctest-name> <target>) instead of "
    "add_test() -- it bakes in the corpus path, adds the test, and sets "
    "SKIP_RETURN_CODE 77 together.")
endif()

list(LENGTH _lazily_registered _lazily_registered_count)
if(_lazily_registered_count EQUAL 0)
  message(FATAL_ERROR
    "no canonical-conformance suite registered -- lazily_add_spec_conformance_test() "
    "was never called, so the corpus is replayed by nothing.")
endif()
message(STATUS
  "lazily-cpp: ${_lazily_registered_count} canonical-conformance suite(s) registered "
  "(corpus ${LAZILY_SPEC_CONFORMANCE_DIR}, absence = ctest SKIP)")

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
