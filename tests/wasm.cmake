# Emscripten/wasm32 test registry (`#lzcppwasm` Phase 2/3).
#
# Included from `tests/CMakeLists.txt` when `EMSCRIPTEN` is set, which then
# returns — the native registry above it is untouched, so this file cannot
# regress the native build.
#
# Which runners are built is NOT written here. It is read from the repo-root
# `wasm-tiers.conf`, the same file `scripts/check-wasm-tiers.sh` reads to
# generate the WASM.md matrix. A separate list here would let the build and the
# published matrix drift apart silently, which is the exact class of defect the
# fixture manifest exists to catch one layer down.
#
# `LAZILY_WASM_TIER` selects the tier: `core` (single-threaded) or `threaded`
# (-pthread). The `native-only` rows are never built here by construction; they
# carry a reason in the conf and land in the matrix as an excluded cell.

if(NOT DEFINED LAZILY_WASM_TIER)
  set(LAZILY_WASM_TIER "core")
endif()
if(NOT LAZILY_WASM_TIER MATCHES "^(core|threaded)$")
  message(FATAL_ERROR
    "LAZILY_WASM_TIER must be 'core' or 'threaded' (got '${LAZILY_WASM_TIER}'). "
    "The 'native-only' tier is not buildable for wasm by definition — see wasm-tiers.conf.")
endif()

set(_tier_conf "${CMAKE_CURRENT_SOURCE_DIR}/../wasm-tiers.conf")
if(NOT EXISTS "${_tier_conf}")
  message(FATAL_ERROR "wasm-tiers.conf not found at ${_tier_conf}")
endif()
# ENCODING UTF-8 is load-bearing, not decoration. Without it `file(STRINGS)`
# treats a non-ASCII byte as a line terminator, so a comment containing an em
# dash arrives as TWO rows — the second of which parses as a malformed entry.
# The malformed-row check below is what surfaced that; keep both.
file(STRINGS "${_tier_conf}" _tier_lines ENCODING UTF-8)

set(_conformance_dir "${CMAKE_CURRENT_SOURCE_DIR}/../../lazily-spec/conformance")

# Emscripten link flags shared by both tiers.
#
# NODERAWFS gives the module direct access to the host filesystem, which is what
# lets the UNMODIFIED fixture loader in test_spec_fixture.hpp open
# ../lazily-spec/conformance/... and append to the coverage manifest. Without it
# the runners would need a wasm-specific loader, and a second loader is a second
# place for the replay to quietly stop being a replay.
#
# EXIT_RUNTIME makes the process exit code the C++ return value, so ctest sees a
# real pass/fail — and so the seam's refusal on an absent corpus (exit 1) still
# reaches ctest as a failure rather than being swallowed by the runtime.
set(_wasm_link_flags
  "-sNODERAWFS=1"
  "-sEXIT_RUNTIME=1"
  "-sALLOW_MEMORY_GROWTH=1"
  "-sSTACK_SIZE=4MB"
  "-fexceptions")

# `-fexceptions` is required, not a preference. emcc defaults to
# `-fignore-exceptions`, which compiles `throw` into an abort and drops every
# `catch`. The library throws on real error paths (a rejected chart definition,
# a malformed frame, an exhausted arena) and several conformance fixtures assert
# exactly those paths, so under the default flags four suites aborted with
# `Aborted(undefined)` instead of reporting. Worse, a suite whose error-path
# fixtures never ran would still have produced a manifest and looked covered.
set(_wasm_compile_flags "-fexceptions")

if(LAZILY_WASM_TIER STREQUAL "threaded")
  # PTHREAD_POOL_SIZE pre-spawns workers: on wasm a thread cannot be created
  # synchronously from the main thread without one, so a pool of zero turns
  # every std::thread into a deadlock rather than an error.
  list(APPEND _wasm_link_flags "-pthread" "-sPTHREAD_POOL_SIZE=8")
endif()

set(_built_targets "")
foreach(_line IN LISTS _tier_lines)
  # Quoted + stripped: an unquoted empty variable collapses the argument list
  # in CMake's `if()`, so a blank separator line reaches the parser below and
  # reports as a malformed row.
  string(STRIP "${_line}" _stripped)
  if("${_stripped}" STREQUAL "" OR "${_stripped}" MATCHES "^#")
    continue()
  endif()
  string(REPLACE "|" ";" _fields "${_line}")
  list(LENGTH _fields _nfields)
  if(_nfields LESS 3)
    message(FATAL_ERROR
      "wasm-tiers.conf: malformed row (expected 'tier|target|ctest-name|reason'): '${_line}'")
  endif()
  list(GET _fields 0 _tier)
  list(GET _fields 1 _target)
  list(GET _fields 2 _ctest_name)
  if(NOT _tier STREQUAL LAZILY_WASM_TIER)
    continue()
  endif()

  add_executable(${_target} ${_target}.cpp)
  target_link_libraries(${_target} PRIVATE lazily)
  target_compile_definitions(${_target} PRIVATE
    LAZILY_SPEC_CONFORMANCE_DIR="${_conformance_dir}")
  target_link_options(${_target} PRIVATE ${_wasm_link_flags})
  target_compile_options(${_target} PRIVATE ${_wasm_compile_flags})
  if(LAZILY_WASM_TIER STREQUAL "threaded")
    target_compile_options(${_target} PRIVATE "-pthread")
  endif()

  # CMAKE_CROSSCOMPILING_EMULATOR is set to node by the Emscripten toolchain, so
  # ctest runs `node <target>.js` without this file naming a runtime.
  # No SKIP_RETURN_CODE: an absent canonical corpus is a hard failure here as it
  # is natively (#lzcppsiblingskipvsfail).
  add_test(NAME ${_ctest_name} COMMAND ${_target})
  list(APPEND _built_targets ${_target})
endforeach()

list(LENGTH _built_targets _built_count)
if(_built_count EQUAL 0)
  message(FATAL_ERROR
    "wasm tier '${LAZILY_WASM_TIER}' built zero runners. A tier that builds "
    "nothing would report an empty matrix as success — see wasm-tiers.conf.")
endif()
message(STATUS "lazily-cpp wasm tier '${LAZILY_WASM_TIER}': ${_built_count} conformance runner(s)")

# The narrow-include contract itself, compiled (`#lzcppwasm` Phase 1).
#
# This is the assertion that `lazily/core.hpp` is genuinely freestanding: a TU
# that includes it and NOTHING else must build for wasm32. A grep over include
# lines could not prove this; a build can.
add_executable(wasm_core_include_check wasm_core_include_check.cpp)
target_link_libraries(wasm_core_include_check PRIVATE lazily)
target_link_options(wasm_core_include_check PRIVATE ${_wasm_link_flags})
target_compile_options(wasm_core_include_check PRIVATE ${_wasm_compile_flags})
if(LAZILY_WASM_TIER STREQUAL "threaded")
  target_compile_options(wasm_core_include_check PRIVATE "-pthread")
endif()
add_test(NAME WasmCoreIncludeCheck COMMAND wasm_core_include_check)
