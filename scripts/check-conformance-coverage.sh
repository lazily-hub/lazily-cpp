#!/usr/bin/env bash
# Positive assertion that canonical conformance fixtures ACTUALLY RAN.
#
# An absence guard ("is ../lazily-spec/conformance present?") cannot catch
# shadowing: a replay that is deleted, renamed, dropped from tests/CMakeLists.txt,
# filtered out by a `ctest -R` selector, or short-circuited still leaves the
# fixture directory on disk and the suite green. Per-binary
# REQUIRE_FIXTURES_LOADED(n) closes half the hole — it proves one executable read
# what it claims to — but it is silent about a binary that no longer runs at all.
#
# lazily-kt closes the other half with a manifest: every fixture read through the
# loader is recorded and flushed on shutdown, and this script asserts the manifest
# is non-trivial and covers every area the binding is expected to replay. The C++
# suite is a set of independent executables rather than one JVM, so each binary
# APPENDS to the manifest named by LAZILY_CONFORMANCE_MANIFEST (see
# tests/test_spec_fixture.hpp) and this script audits the union.
#
# Four independent FIXTURE-level failures are detected:
#   1. the manifest is missing or short   — replays stopped running
#   2. a required AREA contributed nothing — that suite is silently not running
#   3. a fixture exists on disk, is in a required area, and is neither replayed
#      nor listed in KNOWN_UNCOVERED — an upstream corpus addition nobody picked
#      up, which is exactly how a binding drifts out of conformance quietly
#   4. a KNOWN_UNCOVERED entry IS in the replayed manifest — a stale excuse that
#      understates coverage, so the ledger keeps claiming a gap the suite closed
#
# "The file was opened" is one rung short of "the file was replayed", though: a
# fixture carrying several named scenarios can be PARTIALLY replayed and every
# check above stays green, because one scenario is enough to open the file. The
# per-scenario ledger closes that rung and adds three more failures:
#   5. a scenario the fixture declares was never replayed and is not excused
#   6. an excused scenario the run DID replay — a stale excuse
#   7. an excused scenario no opened fixture declares — a stale excuse
# Scenario ids whose fixture carries no `id` and no `name` fall back to the
# stable identifier; a scenario without one is a FAILURE, never booked by
# position (#lzspecscenarioids).
#
# Usage: scripts/check-conformance-coverage.sh [manifest-path]
#
# Environment:
#   LAZILY_SPEC_CONFORMANCE_DIR  override the canonical corpus location
#   MIN_FIXTURES                 override the floor (for debugging only)
#   MAX_LEDGERED_BLOCKS          override the KNOWN_UNBOUND_BLOCKS ceiling (for
#                                debugging only — in a commit, edit the default)

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
manifest="${1:-$repo_root/build/conformance-fixtures-loaded.txt}"
conformance_dir="${LAZILY_SPEC_CONFORMANCE_DIR:-$repo_root/../lazily-spec/conformance}"

# ── corpus-root source hygiene (#lzcorpusrootguards) ───────────────────────
#
# LAZILY_SPEC_CONFORMANCE_DIR only redirects reads that go THROUGH the seam.
# lazily-cpp measured clean on that count — 24/24 areas reddened when the
# override was pointed at a scratch corpus, because every read funnels through
# `spec_conformance_dir()` in tests/test_spec_fixture.hpp, which consults getenv
# before falling back to the compile-time macro. Sibling bindings were not:
# lazily-zig moved 2 of 14 sites and lazily-rs 0 of 25, and both were SILENT,
# because a runner reading the default corpus while believing it was redirected
# is green either way.
#
# This binding already has the strongest structural guard of the family, and it
# is not a text scan: tests/test_spec_fixture.hpp #errors when the macro is
# undefined, so a conformance runner that forgets to register through
# lazily_add_spec_conformance_test() (tests/CMakeLists.txt) cannot even compile.
# That guard has exactly one
# hole — it only reaches translation units that INCLUDE the seam header. A
# source that spells the root itself and never touches the seam builds clean and
# reads the default corpus no matter what the override says.
#
# So this rung scans SOURCES, not the manifest, for anyone spelling the root:
#
#   * single literal      "../lazily-spec/conformance"  (matched on the
#     `lazily-spec/conformance` FRAGMENT, never a fixed count of `../` — the
#     CMake define legitimately spells it with two)
#   * joined segments     path("..") / "lazily-spec" / "conformance"
#   * adjacent literals   "../lazily-spec" "/conformance"   — C++ concatenates
#     these at translation, and it is the form that defeated lazily-go's and
#     lazily-js's guards and lazily-dart's first draft. Literals are therefore
#     compared as a STREAM: each one is re-joined with the next few under both
#     the `/` and the empty separator before the fragment is looked for.
#
# Comments are stripped first, in both languages — some twenty headers and
# runners legitimately quote the corpus path while explaining what they replay,
# and every one of those citations is prose. STRING LITERALS ARE NOT EXEMPT,
# including diagnostic ones: the scan cannot tell a message from a path, so the
# rule is that prose belongs in a comment. That costs nothing today —
# require_spec_checkout's clone hint spells only `../lazily-spec`, not
# the fragment — and it keeps the rule stateable in one sentence.
#
# The allowlist is three entries and each is load-bearing except the seam, which
# is listed as a declaration rather than a necessity: tests/CMakeLists.txt:398
# and tests/wasm.cmake:36 DO spell the root (verified — both fail the scan when
# renamed out of the allowlist), while the seam header currently does not. The
# seam stays listed because it is the one file that is *entitled* to.
#
# It runs BEFORE the missing-corpus SKIP below on purpose. A source-hygiene scan
# needs no corpus, and behind the skip a machine without the sibling checkout
# would sail past it — which is how lazily-gd first mis-placed the same rung.
source_scan_root="${LAZILY_SOURCE_SCAN_ROOT:-$repo_root}"
if ! python3 - "$source_scan_root" <<'CORPUS_ROOT_SCAN'
import os
import re
import subprocess
import sys

root = os.path.abspath(sys.argv[1])
FRAGMENT = "lazily-spec/conformance"

# The read seam, plus the two build files that legitimately DEFINE the macro it
# falls back to. Nothing else may spell the root.
ALLOWED = {
    "tests/test_spec_fixture.hpp",
    "tests/CMakeLists.txt",
    "tests/wasm.cmake",
}

CPP_EXT = (".cpp", ".cc", ".cxx", ".hpp", ".hxx", ".h", ".ipp")
CMAKE_NAMES = ("CMakeLists.txt",)
CMAKE_EXT = (".cmake",)
SKIP_DIRS = {
    ".git", "build", "node_modules", "_deps", "third_party",
    "cmake-build-debug", "cmake-build-release",
}

# Positive-evidence floor, same discipline as MIN_FIXTURES above: a scan that
# quietly stopped finding sources reports OK on every rung it no longer reaches.
MIN_SCANNED = int(os.environ.get("MIN_SCANNED_SOURCES", "100"))

# How many consecutive literals to re-join when hunting the split forms. A path
# assembled from more pieces than this is not something this guard pretends to
# catch; say so rather than implying total reach.
WINDOW = 8

RAW_STRING = re.compile(r'(?:u8|u|U|L)?R"([^ ()\\\t\n]{0,16})\(')
CMAKE_BRACKET_COMMENT = re.compile(r"#\[(=*)\[")


def collapse(text):
    """`a//b` and `a/b` name the same path; normalize before matching."""
    return re.sub(r"/{2,}", "/", text)


def cpp_literals(text):
    """(line, contents) of every string literal, comments and char literals gone."""
    lits = []
    i, n, line = 0, len(text), 1
    while i < n:
        ch = text[i]
        if ch == "\n":
            line += 1
            i += 1
            continue
        if ch in "RuUL":
            m = RAW_STRING.match(text, i)
            if m:
                closer = ")" + m.group(1) + '"'
                start = m.end()
                end = text.find(closer, start)
                if end < 0:
                    end = n
                lits.append((line, text[start:end]))
                line += text.count("\n", i, min(end + len(closer), n))
                i = end + len(closer)
                continue
        if ch == '"':
            j, buf = i + 1, []
            while j < n and text[j] not in ('"', "\n"):
                if text[j] == "\\":
                    buf.append(text[j:j + 2])
                    j += 2
                    continue
                buf.append(text[j])
                j += 1
            lits.append((line, "".join(buf)))
            i = j + 1
            continue
        # A `'` after an identifier/digit character is a C++14 digit separator
        # (1'000'000), not a character literal; treating it as one would swallow
        # the text up to the next quote and blind the scan to whatever is there.
        if ch == "'" and (i == 0 or not (text[i - 1].isalnum() or text[i - 1] == "_")):
            j = i + 1
            while j < n and text[j] not in ("'", "\n"):
                j += 2 if text[j] == "\\" else 1
            i = j + 1
            continue
        if text.startswith("//", i):
            j = text.find("\n", i)
            i = n if j < 0 else j
            continue
        if text.startswith("/*", i):
            j = text.find("*/", i + 2)
            end = n if j < 0 else j + 2
            line += text.count("\n", i, end)
            i = end
            continue
        i += 1
    return lits


def cmake_scan(text):
    """(comment-stripped text, literals). CMake takes unquoted arguments, so the
    whole stripped body is checked, not only the quoted pieces."""
    out, lits = [], []
    i, n, line = 0, len(text), 1
    while i < n:
        ch = text[i]
        if ch == "\n":
            out.append("\n")
            line += 1
            i += 1
            continue
        if ch == "#":
            m = CMAKE_BRACKET_COMMENT.match(text, i)
            if m:
                closer = "]" + m.group(1) + "]"
                end = text.find(closer, m.end())
                end = n if end < 0 else end + len(closer)
            else:
                end = text.find("\n", i)
                end = n if end < 0 else end
            out.append("\n" * text.count("\n", i, end))
            line += text.count("\n", i, end)
            i = end
            continue
        if ch == '"':
            j, buf = i + 1, []
            while j < n and text[j] != '"':
                if text[j] == "\\":
                    buf.append(text[j:j + 2])
                    j += 2
                    continue
                if text[j] == "\n":
                    line += 1
                buf.append(text[j])
                j += 1
            body = "".join(buf)
            lits.append((line, body))
            out.append(body)
            i = j + 1
            continue
        out.append(ch)
        i += 1
    return "".join(out), lits


def joined_hit(lits):
    """First (line) at which a run of up to WINDOW literals spells the fragment
    under either the path-separator or the adjacent-concatenation join."""
    for k in range(len(lits)):
        window = [c for _, c in lits[k:k + WINDOW]]
        for sep in ("/", ""):
            if FRAGMENT in collapse(sep.join(window)):
                return lits[k][0]
    return None


def discover(base):
    files = []
    for flags in (["--cached"], ["--others", "--exclude-standard"]):
        try:
            proc = subprocess.run(
                ["git", "-C", base, "ls-files", "-z"] + flags,
                capture_output=True,
            )
        except OSError:
            files = []
            break
        if proc.returncode != 0:
            files = []
            break
        files += [p for p in proc.stdout.decode("utf-8", "replace").split("\0") if p]
    if files:
        return files
    walked = []
    for dirpath, dirnames, filenames in os.walk(base):
        dirnames[:] = [d for d in dirnames if d not in SKIP_DIRS]
        for name in filenames:
            walked.append(os.path.relpath(os.path.join(dirpath, name), base))
    return walked


scanned = 0
violations = []
for rel in sorted(set(discover(root))):
    base = os.path.basename(rel)
    is_cpp = rel.endswith(CPP_EXT)
    is_cmake = base in CMAKE_NAMES or rel.endswith(CMAKE_EXT)
    if not (is_cpp or is_cmake):
        continue
    if rel in ALLOWED:
        continue
    path = os.path.join(root, rel)
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as handle:
            text = handle.read()
    except OSError:
        continue
    scanned += 1
    if is_cpp:
        hit = joined_hit(cpp_literals(text))
        if hit is not None:
            violations.append((rel, hit))
        continue
    stripped, lits = cmake_scan(text)
    if FRAGMENT in collapse(stripped):
        idx = stripped.find(FRAGMENT)
        line = stripped.count("\n", 0, idx) + 1 if idx >= 0 else 0
        violations.append((rel, line))
        continue
    hit = joined_hit(lits)
    if hit is not None:
        violations.append((rel, hit))

if scanned == 0:
    sys.stderr.write(
        "ERROR: the corpus-root source scan examined NOTHING under '%s'.\n"
        "       Zero files is not a clean tree, it is a scan that reached nothing —\n"
        "       exactly the vacuous pass this rung exists to refuse.\n" % root
    )
    sys.exit(1)

if scanned < MIN_SCANNED:
    sys.stderr.write(
        "ERROR: the corpus-root source scan examined only %d file(s) under '%s',\n"
        "       expected >= %d. Discovery narrowed; the rungs it no longer reaches\n"
        "       would report OK forever. Do not lower MIN_SCANNED_SOURCES to fix this.\n"
        % (scanned, root, MIN_SCANNED)
    )
    sys.exit(1)

if violations:
    for rel, line in violations:
        sys.stderr.write(
            "ERROR: %s:%d spells the canonical corpus root itself.\n" % (rel, line)
        )
    sys.stderr.write(
        "       LAZILY_SPEC_CONFORMANCE_DIR cannot redirect a read that never goes\n"
        "       through the seam, and such a translation unit does not include\n"
        "       tests/test_spec_fixture.hpp, so its #error cannot see it either. The\n"
        "       runner would replay the DEFAULT corpus while believing it was\n"
        "       redirected, and stay green either way. Read fixtures through\n"
        "       lazily_test::spec_fixture_text (#lzcorpusrootguards).\n"
    )
    sys.exit(1)

print(
    "corpus-root source hygiene OK: %d source file(s) scanned, none spells "
    "'%s' outside the seam" % (scanned, FRAGMENT)
)
CORPUS_ROOT_SCAN
then
  echo "conformance coverage FAILED: corpus-root source hygiene" >&2
  exit 1
fi

# Absence of the sibling checkout is a hard FAILURE, consistent with the suites
# themselves (require_spec_checkout in tests/test_spec_fixture.hpp) and with
# every other guard in this repo that reads the corpus (#lzcppsiblingskipvsfail).
#
# It used to exit 0 with a SKIP line. That was measured, not argued: with the
# corpus absent, 25 of 62 ctest targets run and 37 skip, 0 of 139 canonical
# fixtures and 0 of 149 scenarios are replayed, and this guard's whole job — the
# MIN_FIXTURES / MIN_SCENARIOS floors, the per-area coverage audit, the
# declared-vs-replayed scenario ledger — is skipped along with them. `make check`
# is the pre-commit gate; a green there over that state is a false green, and it
# is the one that let 9b0ff08 pass locally and land red on CI.
if [[ ! -d "$conformance_dir" ]]; then
  echo "ERROR: canonical conformance corpus not found at '$conformance_dir'." >&2
  echo "       git clone https://github.com/lazily-hub/lazily-spec.git ../lazily-spec" >&2
  echo "       (or point LAZILY_SPEC_CONFORMANCE_DIR at a checkout)" >&2
  echo "       This is a hard failure, not a skip: without the corpus this guard" >&2
  echo "       audits nothing and would report OK over an unreplayed suite." >&2
  echo "conformance coverage FAILED: canonical corpus absent" >&2
  exit 1
fi

# Minimum total DISTINCT fixtures replayed. EXACT: 139 is what the suite really
# replays, with no margin. NEVER lower it to make a red build green — a drop
# means a replay stopped running, and that is the finding, not the floor.
#
# When replays are added, set this to the number the guard REPORTS afterwards.
# Do NOT add "the N I just added" to the old value (#lzscenariofloordrift): the
# fixture-by-fixture arithmetic this comment replaced did exactly that, adding
# each delta on top of a floor that already sat under reality, so the gap only
# ever widened. It had reached 127 against an actual 137 — ten replays could
# have been deleted while the guard kept printing OK, which is precisely the
# "deleting a runner hides behind older aggregate growth" failure that
# arithmetic was written to prevent. MIN_SCENARIOS below was pulled up to its
# actual figure for the same reason; this floor is now kept the same way.
#
# 137 -> 139: lazily-spec 39df4b3 (#lzspecoutoforderfixtures) added
# lossless-tree/apply_update_advances_counter.json and
# lossless-tree/out_of_order_delivery_buffers.json, and
# tests/test_lossless_tree_conformance.cpp now replays both. They are published,
# so CI's clone carries them. Set to what the guard REPORTED after the change,
# not to 137+2 — those happen to agree here only because nothing else moved.
#
# 139 -> 140: lazily-spec v0.38.0 added the latest-durable projection fixture,
# replayed by tests/test_latest_durable_projection.cpp.
#
# 140 -> 143: the replay-equivalence proof (`#lzreplaycpp`) landed, and
# tests/test_replay_conformance.cpp now opens all three of lazily-spec's
# `replay/` fixtures. That is what moved `replay` out of EXCUSED_AREAS and
# emptied its three KNOWN_UNCOVERED entries. Set to what the guard REPORTED
# after the change, not to 140+3.
#
# Confirmed by a local green `make check`. Verified exact: 144 fails this floor.
MIN_FIXTURES="${MIN_FIXTURES:-143}"

# Areas lazily-cpp is expected to replay. An area belongs here once a runner
# opens its fixtures through `spec_fixture_text`; listing an area the binding
# does not read at all would make this guard permanently red, and omitting one it
# does read would let that runner vanish unnoticed — which is the whole point.
#
# This list carried its complement as PROSE until #lzledgeragreementaudit: a
# comment naming the areas deliberately left out, with reasons. That was the same
# defect this script exists to catch, one level up. The completeness loop below
# walks only the arrays, so an area in neither one is invisible to EVERY rung —
# not replayed, not excused, not counted, not named. `egress` sat in exactly that
# state with four fixtures: absent from this list, absent from KNOWN_UNCOVERED,
# and absent even from the prose. Nothing here could have reported it, and the
# guard printed OK. Every sibling binding names those four in KNOWN_UNCOVERED;
# this one alone had no ledger entry to be stale.
#
# So the complement is now an ARRAY, checked in both directions against the
# corpus and the manifest, and every fixture inside it is named in
# KNOWN_UNCOVERED with a reason. A prose comment cannot be falsified by a run.
REQUIRED_AREAS=(
codec
collections
coordination
crdt-tree
  distributed
  egress
  familysync
ingress
ipc
lossless-tree
  materialization
  membership
  message-passing
  presence
  rateshape
  reactive-graph
  receipts
  reliable-sync
  replay
  resilience
  service
  signaling
  statechart
  stdlib
  temporal
  windowing
)

# Areas this binding replays NOTHING from. The complement of REQUIRED_AREAS, and
# the two together must cover the corpus exactly — see the partition check below.
#
# An entry here excuses the AREA from the "some fixture replayed" rung only. It
# does NOT excuse the fixtures: each one is still named in KNOWN_UNCOVERED with
# its own reason, so the stale-ledger arms keep verifying it exists upstream and
# stays unread. An area is the unit of "no runner"; a fixture is the unit of the
# claim.
EXCUSED_AREAS=(
  # IPC wire snapshots of the agent-doc state projection — an application schema
  # carried on the IPC plane, not a binding-level reactive concern.
  agent-doc
  # The experimental protobuf-v1 generator pilot is Rust/Kotlin/TypeScript.
  protobuf
)

# Fixtures inside a REQUIRED or EXCUSED area that this binding does NOT yet
# replay, each with the reason it is outstanding. This list is the honest ledger
# of the gap: a fixture is either replayed or named here, never silently absent.
# Entries are verified BOTH to still exist on disk and to still be unread, so
# neither a fixture renamed/deleted upstream nor one this suite started replaying
# can rot into a permanent excuse.
#
# Shrinking this list is the work. Growing it requires a stated reason.
KNOWN_UNCOVERED=(
  # agent-doc — IPC wire snapshots of the agent-doc state projection, an
  # application schema on the IPC plane rather than a binding-level concern.
  "agent-doc/delta_agent_doc_state.json"
  "agent-doc/snapshot_agent_doc_state.json"
  # Register CRDTs (LWW / MV / PnCounter + the CellCrdt projection bit) are
  # implemented here, but this binding has no canonical replay for the new
  # registers corpus yet; the Registers coverage row is `~` until it does.
  "collections/registers_convergence.json"
  # egress — the legacy FIFO stream contract remains distinct from the
  # latest-durable per-key projection implemented by this binding.
  "egress/egress_generation_fence.json"
  "egress/egress_inflight_window.json"
  "egress/egress_ordered_ack.json"
  "egress/egress_retry_budget.json"
# ipc — the C++ arena is an in-process host and does not implement the
# canonical mapped 40-byte LZSH header asserted by this fixture.
"arena_blob.json"
# protobuf — the experimental v1 generator pilot is Rust/Kotlin/TypeScript;
  # this binding must negotiate the capability before replaying the typed trace.
  "protobuf/graph_boundary_traces.json"
  # reliable-sync — the remaining three need distinct outbox coalescing, lease
  # eviction, and journal-decoder runners.
  "reliable-sync/coalesce_bounds_outbox.json"
  "reliable-sync/liveness_lease_eviction.json"
  # The canonical journal-decoder trace has no C++ replay runner yet.
  "reliable-sync/outbox_journal_decode.json"
  # signaling — anti_spoof_session.json IS replayed by
  # test_signaling_conformance.cpp; frames.json needs signaling wire serde,
  # which this binding does not have.
  "signaling/frames.json"
)

# Assertion-block SITES of an opened fixture that NO tracker binds, each with the
# reason it cannot be bound (#lzcppblockwalk). Kept beside KNOWN_UNCOVERED so
# there is one place to read what this binding does not prove.
#
# Written per SITE and not per fixture on purpose: when the missing op lands,
# each entry fails as STALE and has to be deleted one at a time, which is what
# stops an excuse outliving the gap it describes. A per-fixture entry would go
# on excusing the whole file while nine of its ten steps had started binding.
#
# Every entry below is a step of one of the six fixtures
# tests/test_reactive_graph_conformance.cpp already records in
# EXPECTED_UNSUPPORTED / PARKED. The replay stops on an op or a novel assertion
# key this binding does not implement, so the steps past that point never run
# and their `expect` blocks are UNREACHABLE rather than unbound. An unbindable
# block belongs here as an excuse the guard re-reads every run, never as a
# runner fabricated to manufacture coverage.
#
# Open and close parens are on their OWN LINES even when this is empty:
# lazily-spec's check-corpus-floors.mjs finds an array by `NAME=(` and then
# scans for the next line beginning `)`, so a same-line `NAME=()` hands it the
# close of whichever array comes next and every entry in between is misread.
#
# This ledger may only SHRINK: MAX_LEDGERED_BLOCKS below caps it at the count it
# carries today, because the set equality that checks it is satisfied by any
# CONSISTENT pair and so cannot see a commit that detaches binds and adds the
# matching entries (#lzledgerceiling).
#
# Format: "fixture|where|reason".
KNOWN_UNBOUND_BLOCKS=(
  "reactive-graph/exact_fold_paths_stay_exact.json|steps[2].expect|the fixture drives a merge_cell op and this runner's op vocabulary has no merge-feed node kind, so test_reactive_graph_conformance.cpp records it in EXPECTED_UNSUPPORTED, the replay stops on that op, and this step's expect block is UNREACHABLE rather than unbound"
  "reactive-graph/exact_fold_paths_stay_exact.json|steps[3].expect|the fixture drives a merge_cell op and this runner's op vocabulary has no merge-feed node kind, so test_reactive_graph_conformance.cpp records it in EXPECTED_UNSUPPORTED, the replay stops on that op, and this step's expect block is UNREACHABLE rather than unbound"
  "reactive-graph/exact_fold_paths_stay_exact.json|steps[4].expect|the fixture drives a merge_cell op and this runner's op vocabulary has no merge-feed node kind, so test_reactive_graph_conformance.cpp records it in EXPECTED_UNSUPPORTED, the replay stops on that op, and this step's expect block is UNREACHABLE rather than unbound"
  "reactive-graph/feedback_drain_bound_reports_exhaustion.json|steps[1].expect|the fixture pins the novel drain_exhausted / writes_own_cone keys this runner does not model, so test_reactive_graph_conformance.cpp records it in both EXPECTED_UNSUPPORTED and PARKED, the replay never enters the step, and this expect block is UNREACHABLE rather than unbound"
  "reactive-graph/feedback_drain_bound_reports_exhaustion.json|steps[2].expect|the fixture pins the novel drain_exhausted / writes_own_cone keys this runner does not model, so test_reactive_graph_conformance.cpp records it in both EXPECTED_UNSUPPORTED and PARKED, the replay never enters the step, and this expect block is UNREACHABLE rather than unbound"
  "reactive-graph/feedback_drain_bound_reports_exhaustion.json|steps[3].expect|the fixture pins the novel drain_exhausted / writes_own_cone keys this runner does not model, so test_reactive_graph_conformance.cpp records it in both EXPECTED_UNSUPPORTED and PARKED, the replay never enters the step, and this expect block is UNREACHABLE rather than unbound"
  "reactive-graph/merge_cell_acquires_no_dependency_edge.json|steps[1].expect|the fixture drives a merge_cell op and this runner's op vocabulary has no merge-feed node kind, so test_reactive_graph_conformance.cpp records it in EXPECTED_UNSUPPORTED, the replay stops on that op, and this step's expect block is UNREACHABLE rather than unbound"
  "reactive-graph/merge_cell_acquires_no_dependency_edge.json|steps[2].expect|the fixture drives a merge_cell op and this runner's op vocabulary has no merge-feed node kind, so test_reactive_graph_conformance.cpp records it in EXPECTED_UNSUPPORTED, the replay stops on that op, and this step's expect block is UNREACHABLE rather than unbound"
  "reactive-graph/merge_cell_acquires_no_dependency_edge.json|steps[3].expect|the fixture drives a merge_cell op and this runner's op vocabulary has no merge-feed node kind, so test_reactive_graph_conformance.cpp records it in EXPECTED_UNSUPPORTED, the replay stops on that op, and this step's expect block is UNREACHABLE rather than unbound"
  "reactive-graph/merge_cell_acquires_no_dependency_edge.json|steps[4].expect|the fixture drives a merge_cell op and this runner's op vocabulary has no merge-feed node kind, so test_reactive_graph_conformance.cpp records it in EXPECTED_UNSUPPORTED, the replay stops on that op, and this step's expect block is UNREACHABLE rather than unbound"
  "reactive-graph/merge_feed_through_a_formula_coalesces.json|steps[2].expect|the fixture drives a merge_cell op and this runner's op vocabulary has no merge-feed node kind, so test_reactive_graph_conformance.cpp records it in EXPECTED_UNSUPPORTED, the replay stops on that op, and this step's expect block is UNREACHABLE rather than unbound"
  "reactive-graph/merge_feed_through_a_formula_coalesces.json|steps[3].expect|the fixture drives a merge_cell op and this runner's op vocabulary has no merge-feed node kind, so test_reactive_graph_conformance.cpp records it in EXPECTED_UNSUPPORTED, the replay stops on that op, and this step's expect block is UNREACHABLE rather than unbound"
  "reactive-graph/merge_feed_through_a_formula_coalesces.json|steps[4].expect|the fixture drives a merge_cell op and this runner's op vocabulary has no merge-feed node kind, so test_reactive_graph_conformance.cpp records it in EXPECTED_UNSUPPORTED, the replay stops on that op, and this step's expect block is UNREACHABLE rather than unbound"
  "reactive-graph/merge_feed_through_a_formula_coalesces.json|steps[5].expect|the fixture drives a merge_cell op and this runner's op vocabulary has no merge-feed node kind, so test_reactive_graph_conformance.cpp records it in EXPECTED_UNSUPPORTED, the replay stops on that op, and this step's expect block is UNREACHABLE rather than unbound"
  "reactive-graph/merge_feed_through_a_formula_coalesces.json|steps[6].expect|the fixture drives a merge_cell op and this runner's op vocabulary has no merge-feed node kind, so test_reactive_graph_conformance.cpp records it in EXPECTED_UNSUPPORTED, the replay stops on that op, and this step's expect block is UNREACHABLE rather than unbound"
  "reactive-graph/merge_feed_through_a_formula_coalesces.json|steps[7].expect|the fixture drives a merge_cell op and this runner's op vocabulary has no merge-feed node kind, so test_reactive_graph_conformance.cpp records it in EXPECTED_UNSUPPORTED, the replay stops on that op, and this step's expect block is UNREACHABLE rather than unbound"
  "reactive-graph/merge_folds_synchronously_in_batch.json|steps[1].expect|the fixture drives a merge_cell op and this runner's op vocabulary has no merge-feed node kind, so test_reactive_graph_conformance.cpp records it in EXPECTED_UNSUPPORTED, the replay stops on that op, and this step's expect block is UNREACHABLE rather than unbound"
  "reactive-graph/merge_folds_synchronously_in_batch.json|steps[2].expect|the fixture drives a merge_cell op and this runner's op vocabulary has no merge-feed node kind, so test_reactive_graph_conformance.cpp records it in EXPECTED_UNSUPPORTED, the replay stops on that op, and this step's expect block is UNREACHABLE rather than unbound"
  "reactive-graph/merge_folds_synchronously_in_batch.json|steps[3].expect|the fixture drives a merge_cell op and this runner's op vocabulary has no merge-feed node kind, so test_reactive_graph_conformance.cpp records it in EXPECTED_UNSUPPORTED, the replay stops on that op, and this step's expect block is UNREACHABLE rather than unbound"
  "reactive-graph/merge_per_settled_cone_not_per_write.json|steps[1].expect|the fixture drives a merge_cell op and this runner's op vocabulary has no merge-feed node kind, so test_reactive_graph_conformance.cpp records it in EXPECTED_UNSUPPORTED, the replay stops on that op, and this step's expect block is UNREACHABLE rather than unbound"
  "reactive-graph/merge_per_settled_cone_not_per_write.json|steps[2].expect|the fixture drives a merge_cell op and this runner's op vocabulary has no merge-feed node kind, so test_reactive_graph_conformance.cpp records it in EXPECTED_UNSUPPORTED, the replay stops on that op, and this step's expect block is UNREACHABLE rather than unbound"
  "reactive-graph/merge_per_settled_cone_not_per_write.json|steps[3].expect|the fixture drives a merge_cell op and this runner's op vocabulary has no merge-feed node kind, so test_reactive_graph_conformance.cpp records it in EXPECTED_UNSUPPORTED, the replay stops on that op, and this step's expect block is UNREACHABLE rather than unbound"
  "reactive-graph/merge_per_settled_cone_not_per_write.json|steps[4].expect|the fixture drives a merge_cell op and this runner's op vocabulary has no merge-feed node kind, so test_reactive_graph_conformance.cpp records it in EXPECTED_UNSUPPORTED, the replay stops on that op, and this step's expect block is UNREACHABLE rather than unbound"
  "reactive-graph/merge_per_settled_cone_not_per_write.json|steps[5].expect|the fixture drives a merge_cell op and this runner's op vocabulary has no merge-feed node kind, so test_reactive_graph_conformance.cpp records it in EXPECTED_UNSUPPORTED, the replay stops on that op, and this step's expect block is UNREACHABLE rather than unbound"
  "reactive-graph/merge_per_settled_cone_not_per_write.json|steps[6].expect|the fixture drives a merge_cell op and this runner's op vocabulary has no merge-feed node kind, so test_reactive_graph_conformance.cpp records it in EXPECTED_UNSUPPORTED, the replay stops on that op, and this step's expect block is UNREACHABLE rather than unbound"
)

# ── per-scenario ledger ────────────────────────────────────────────────────
#
# Same shape, one rung down: a scenario of an OPENED fixture is either replayed
# or excused here with a reason, never silently absent. Kept beside
# KNOWN_UNCOVERED on purpose, so there is one place to read what this binding
# does not prove.
#
# Fixtures this binding never opens contribute nothing here — their gap is
# already stated above, and restating it per scenario would double-count it.
SCENARIO_EXCUSES=()

# excuse_scenario <fixture> <scenario-id> <reason>
excuse_scenario() {
  local fixture="$1" id="$2" reason="${3:-}"
  if [[ -z "$reason" ]]; then
    echo "ERROR: excuse_scenario '$fixture' '$id' has no reason — an excuse without" >&2
    echo "       one is the allowlist this ledger exists to replace." >&2
    exit 1
  fi
  SCENARIO_EXCUSES+=("${fixture}"$'\t'"${id}"$'\t'"${reason}")
}

# Minimum DISTINCT scenarios replayed, the per-scenario twin of MIN_FIXTURES.
# Without it a ledger that stopped recording entirely would leave the
# declared-vs-replayed comparison vacuously satisfied on both sides.
#
# The floor sat at 83 while the run replayed 107, so a quarter of the ledger
# could have stopped recording without this check noticing — a slack floor is a
# floor that has stopped guarding. Pulled up to the actual figure, matching how
# MIN_FIXTURES is kept. The +19 that took it to 137 came from converting
# multi_epoch_delta.json's two scenarios, outbox_store_protocol.json's four,
# four reliable-sync fixture families' ten, and familysync's three scenarios
# from hand transcriptions/excuses into actual fixture-driven replays. The +7
# that takes it to 144 is corpus growth this binding already replays with no
# runner change.
#
# The floor tracks the corpus CI ACTUALLY GETS: CI clones lazily-spec at its
# published main, so a scenario that exists only in a local sibling checkout
# cannot be part of the floor. A tree carrying unpublished spec work replays
# MORE than the floor and still passes — the check is a minimum. Raise this once
# the scenarios land upstream, never to match a working tree.
#
# 144 -> 146: the two SeqCrdt fork-clock scenarios this floor was deliberately
# held back for are now published (lazily-spec f3246a1, #lzspecforkclockfixture),
# so CI's clone carries them and they are guaranteed rather than local-only.
#
# 146 -> 147: textcrdt_convergence.json gained
# gc_keeps_a_tombstone_that_is_still_a_left_origin (lazily-spec 34caf9c,
# #lzspecgcreferencedtombstone) — an INTERIOR delete, so the survivor still
# names the tombstone as its left origin and a conservative collector must take
# 0. The pre-existing gc scenario deletes the LAST character, so nothing
# referenced its tombstone and a collector that ignored origins entirely stayed
# green; the new scenario is the one that bites. It is published, so CI's clone
# carries it and this binding replays it with no runner change.
#
# NEVER lower it to make a red build green. When replays are added, set this to
# the number the guard REPORTS afterwards — never the old value plus however
# many you added, which is how MIN_FIXTURES above drifted ten below reality
# (#lzscenariofloordrift).
#
# 147 -> 149: lazily-spec 39df4b3 (#lzspecoutoforderfixtures) published the two
# fixtures that pin `apply_update`'s counter and buffering rules —
# apply_update_advances_counter.json's post_sync_write_outranks_ingested_stamp
# and out_of_order_delivery_buffers.json's reversed_batch_drains_through_buffer,
# one scenario each. They are the corpus-level form of this binding's
# tests/test_lossless_tree_apply_update_{counter,buffering}.cpp, and replaying
# the second one needed the `deliver.order` selector
# (tests/test_lossless_tree_deliver.hpp).
#
# 149 -> 151 (#lzcppscenfloor): pure drift repair, not new replay work. The two
# scenarios are egress/latest_durable_projection.json's
# latest_projection_supersedes_pending_without_false_ack and
# keyed_single_flight_reconnect_fences_stale_actor, published in lazily-spec
# 1c388a5 (v0.38.0). MIN_FIXTURES was moved 139 -> 140 for that same fixture and
# this floor was not, so the guard sat two under reality — two replays could
# have stopped recording while it printed OK, which is exactly the slack the
# paragraph above says is a floor that has stopped guarding. Five sibling
# bindings re-pinned during the same sweep; this one was held back for scope.
#
# This floor is EXACT today: 151 declared, 151 replayed, confirmed by a local
# green `make check`, and verified by watching 152 fail it. MIN_FIXTURES was
# re-checked in the same pass and is still exact at 143 (144 fails it).
MIN_SCENARIOS="${MIN_SCENARIOS:-151}"

if [[ ! -f "$manifest" ]]; then
  echo "ERROR: no conformance manifest at '$manifest' — the fixture replays did not run at all." >&2
  echo "       Run the suite with LAZILY_CONFORMANCE_MANIFEST set (see the Makefile's \`test\` target)." >&2
  exit 1
fi

# Every binary appends, so one fixture can appear several times (three runners
# read the materialization corpus). Count DISTINCT fixtures: the question is
# corpus coverage, not read count.
replayed="$(mktemp)"
declared_scenarios="$(mktemp)"
replayed_scenarios="$(mktemp)"
unidentified_scenarios="$(mktemp)"
trap 'rm -f "$replayed" "$declared_scenarios" "$replayed_scenarios" "$unidentified_scenarios"' EXIT
sort -u "$manifest" | grep . | grep -v '^@' > "$replayed" || true
count="$(wc -l < "$replayed" | tr -d ' ')"

# Scenario records are tab-delimited and tag-prefixed (tests/test_spec_fixture.hpp).
# `declared` is what the corpus on disk carries for every fixture this run
# opened; `replayed` is what a runner actually entered.
awk -F'\t' '$1=="@declared"   {print $2 "\t" $3}' "$manifest" | sort -u > "$declared_scenarios"
awk -F'\t' '$1=="@replayed"   {print $2 "\t" $3}' "$manifest" | sort -u > "$replayed_scenarios"
awk -F'\t' '$1=="@unidentified" {print $2 "\t" $3}' "$manifest" | sort -u > "$unidentified_scenarios"
scenario_count="$(wc -l < "$replayed_scenarios" | tr -d ' ')"

status=0

if (( count < MIN_FIXTURES )); then
  echo "ERROR: only $count distinct conformance fixtures replayed, expected >= $MIN_FIXTURES." >&2
  echo "       A replay was removed, renamed, or short-circuited. Do not lower MIN_FIXTURES to fix this." >&2
  status=1
fi

for area in "${REQUIRED_AREAS[@]}"; do
if [[ "$area" == "ipc" ]]; then
area_pattern='^(arena_blob|snapshot_.*|delta_.*)\.json$'
else
area_pattern="^${area}/"
fi
if ! grep -Eq "$area_pattern" "$replayed"; then
echo "ERROR: no fixtures replayed from conformance area '$area' — that suite is silently not running." >&2
status=1
fi
done

# The two area arrays must PARTITION the corpus (#lzledgeragreementaudit). Every
# other rung in this script iterates an array, so a corpus directory named in
# neither is unreachable by all of them at once: its fixtures are never demanded,
# never excused, and never counted, and the guard still prints OK. That is not a
# hypothetical — `egress/` shipped four fixtures in exactly that state, and the
# only thing that ever mentioned the omission was a prose comment no run could
# falsify.
#
# Checked in both directions. A NEW upstream area fails closed here rather than
# vanishing, and an array entry naming a directory the corpus no longer has fails
# as stale. `ipc` is the one deliberate non-directory: its fixtures live at the
# corpus root and are matched by name above, so it is exempt from the second arm.
corpus_areas="$(cd "$conformance_dir" && find . -mindepth 1 -maxdepth 1 -type d -printf '%f\n' | sort)"
while IFS= read -r area; do
  [[ -n "$area" ]] || continue
  listed=0
  for known in "${REQUIRED_AREAS[@]}" "${EXCUSED_AREAS[@]}"; do
    [[ "$known" == "$area" ]] && { listed=1; break; }
  done
  if (( listed == 0 )); then
    echo "ERROR: conformance area '$area' is in neither REQUIRED_AREAS nor EXCUSED_AREAS." >&2
    echo "       Every rung below walks those arrays, so this area's fixtures are invisible" >&2
    echo "       to all of them — not replayed, not excused, not counted. Add it to" >&2
    echo "       REQUIRED_AREAS once a runner opens it, or to EXCUSED_AREAS with a reason" >&2
    echo "       and each of its fixtures in KNOWN_UNCOVERED." >&2
    status=1
  fi
done <<< "$corpus_areas"

for area in "${REQUIRED_AREAS[@]}" "${EXCUSED_AREAS[@]}"; do
  [[ "$area" == "ipc" ]] && continue
  if [[ ! -d "$conformance_dir/$area" ]]; then
    echo "ERROR: area '$area' is listed here but is not in the canonical corpus." >&2
    echo "       It was renamed or removed upstream — prune the entry." >&2
    status=1
  fi
done

# An EXCUSED area that DID replay something is a stale excuse, and it understates
# coverage exactly the way a stale KNOWN_UNCOVERED entry does. Same both-directions
# rule, same reason: an excuse nobody can falsify is not evidence.
for area in "${EXCUSED_AREAS[@]}"; do
  if grep -Eq "^${area}/" "$replayed"; then
    echo "ERROR: EXCUSED_AREAS lists '$area', but the suite DID replay from it." >&2
    echo "       Move it to REQUIRED_AREAS and prune its KNOWN_UNCOVERED entries so the" >&2
    echo "       ledger stops claiming a gap this binding closed." >&2
    status=1
  fi
done

# Stale-ledger check, both directions. An excuse is only honest while the fixture
# still EXISTS upstream and is still UNREAD by this suite:
#
#   * gone from the corpus  — the ledger excuses a name that means nothing
#   * present in the manifest — the ledger claims a gap the suite already closed,
#     which understates coverage and lets a stale excuse rot into permanence
#
# The second arm is the one that matters for drift: nothing else in this script
# ever reads KNOWN_UNCOVERED against the manifest. The completeness check below
# consults the list only for fixtures that are ABSENT from the manifest, so a
# fixture that starts being replayed silently keeps its excuse forever. Both arms
# feed the same `status` counter as the MIN_FIXTURES and REQUIRED_AREAS checks, so
# a stale excuse fails the build exactly like a missing replay does.
#
# Membership uses `grep -qxF`, byte-identical to the covered-check in the
# completeness loop, so an entry can never be "replayed" for one check and
# "uncovered" for the other.
for excused in "${KNOWN_UNCOVERED[@]}"; do
  if [[ ! -f "$conformance_dir/$excused" ]]; then
    echo "ERROR: KNOWN_UNCOVERED lists '$excused', which is not in the canonical corpus." >&2
    echo "       It was renamed or deleted upstream — prune the entry." >&2
    status=1
  elif grep -qxF "$excused" "$replayed"; then
    echo "ERROR: KNOWN_UNCOVERED lists '$excused', but it IS replayed — the excuse is stale." >&2
    echo "       A runner now opens this fixture; prune the entry so the ledger stops" >&2
    echo "       understating coverage." >&2
    status=1
  fi
done

# Completeness: every canonical fixture in a required OR excused area is
# replayed, or named in KNOWN_UNCOVERED. This is the check that catches the
# corpus GROWING. Excused areas are walked too — "no runner for this area" is not
# a licence to stop accounting for the fixtures in it, and the partition check
# above is what makes this loop's reach equal to the corpus.
for area in "${REQUIRED_AREAS[@]}" "${EXCUSED_AREAS[@]}"; do
if [[ "$area" == "ipc" ]]; then
fixture_ids="$(cd "$conformance_dir" &&
  find . -maxdepth 1 -type f \( -name 'arena_blob.json' -o -name 'snapshot_*.json' -o -name 'delta_*.json' \) \
    -printf '%f\n' | sort)"
else
[[ -d "$conformance_dir/$area" ]] || continue
fixture_ids="$(cd "$conformance_dir/$area" &&
  find . -maxdepth 1 -type f -name '*.json' -printf "${area}/%f\n" | sort)"
fi
while IFS= read -r id; do
[[ -n "$id" ]] || continue
grep -qxF "$id" "$replayed" && continue
excused=0
    for known in "${KNOWN_UNCOVERED[@]}"; do
      [[ "$known" == "$id" ]] && { excused=1; break; }
    done
    if (( excused == 0 )); then
      echo "ERROR: canonical fixture '$id' exists on disk but is never replayed." >&2
      echo "       Write a runner that reads it, or add it to KNOWN_UNCOVERED with a reason." >&2
status=1
fi
done <<< "$fixture_ids"
done

# ── per-scenario accounting ────────────────────────────────────────────────
#
# Everything above asks whether a FILE was opened. These three checks ask
# whether each SCENARIO inside an opened file was entered, which is the failure
# a file-level manifest cannot see: replay one of four scenarios and the fixture
# still counts as covered.

if (( scenario_count < MIN_SCENARIOS )); then
  echo "ERROR: only $scenario_count distinct scenarios replayed, expected >= $MIN_SCENARIOS." >&2
  echo "       A replay loop stopped recording, or a runner stopped running. Do not lower" >&2
  echo "       MIN_SCENARIOS to fix this." >&2
  status=1
fi

scenario_is_excused() {
  local pair="$1" entry
  for entry in ${SCENARIO_EXCUSES[@]+"${SCENARIO_EXCUSES[@]}"}; do
    [[ "${entry%$'\t'*}" == "$pair" ]] && return 0
  done
  return 1
}

# Forward: every scenario an opened fixture carries was replayed, or excused.
while IFS= read -r pair; do
  [[ -n "$pair" ]] || continue
  grep -qxF "$pair" "$replayed_scenarios" && continue
  scenario_is_excused "$pair" && continue
  echo "ERROR: scenario '${pair#*$'\t'}' of '${pair%%$'\t'*}' is carried by the fixture but" >&2
  echo "       was never replayed. The file was opened, so every file-level check passed" >&2
  echo "       while this scenario proved nothing. Replay it, or excuse_scenario it with a" >&2
  echo "       reason beside KNOWN_UNCOVERED." >&2
  status=1
done < "$declared_scenarios"

# Reverse, both arms, exactly as KNOWN_UNCOVERED is checked: an excuse is only
# honest while the scenario is still declared upstream AND still unreplayed.
for entry in ${SCENARIO_EXCUSES[@]+"${SCENARIO_EXCUSES[@]}"}; do
  pair="${entry%$'\t'*}"
  if grep -qxF "$pair" "$replayed_scenarios"; then
    echo "ERROR: scenario '${pair#*$'\t'}' of '${pair%%$'\t'*}' is excused, but this run DID" >&2
    echo "       replay it — the excuse is stale and now hides nothing. Delete it." >&2
    status=1
  elif ! grep -qxF "$pair" "$declared_scenarios"; then
    echo "ERROR: scenario '${pair#*$'\t'}' of '${pair%%$'\t'*}' is excused, but no fixture this" >&2
    echo "       run opened carries that id — the excuse was renamed away upstream or names a" >&2
    echo "       fixture nobody reads. Delete it." >&2
    status=1
  fi
done

if (( status != 0 )); then
  exit 1
fi

# A scenario with no `id` and no `name` is a corpus defect, not an id to invent
# (#lzspecscenarioids). Booking it by POSITION makes every ledger entry for that
# fixture order-dependent: a reordering upstream silently rebinds them all, and
# the guard compares "index 1 was replayed" against whatever now sits at index 1
# and agrees with itself.
if [[ -s "$unidentified_scenarios" ]]; then
  while IFS= read -r pair; do
    [[ -n "$pair" ]] || continue
    echo "ERROR: ${pair%%$'\t'*} scenario at index ${pair#*$'\t'} carries neither 'id' nor" >&2
    echo "       'name'. The ledger would record it by POSITION, which silently rebinds" >&2
    echo "       on a corpus reorder. Give it a stable id upstream in lazily-spec" >&2
    echo "       (#lzspecscenarioids)." >&2
  done < "$unidentified_scenarios"
  echo "conformance coverage FAILED: $(wc -l < "$unidentified_scenarios" | tr -d ' ')" \
       "unidentified scenario(s)" >&2
  exit 1
fi

# ── rung 0's MAGNITUDE (#lzblockmagnitudeaudit) ────────────────────────────
#
# tests/test_assertion_keys.hpp's BindLedger closes the rung: every top-level
# `assertions` block the loader hands out must be BOUND to an AssertionKeys by
# some runner, or its destructor aborts. That guard is POSITIVE about bind and
# silent about magnitude — zero declared blocks means zero unbound blocks, and
# it prints nothing while comparing nothing (#lzvacuousrun). A loader that
# stopped declaring, a `declare_assertion_block` call deleted from
# `spec_fixture_text`, a walk narrowed by an edit: all of them take the ledger
# to empty and abort nothing.
#
# So the inventory is compared against a number this run cannot influence. The
# expectation is DERIVED from two things this repo can be held to: (a) the
# canonical corpus DIRECTORY LISTING under $conformance_dir, and (b) this
# binding's own committed `KNOWN_UNCOVERED` ledger — the same subtraction
# MIN_FIXTURES is checked against one rung up. Listing minus ledger is the
# opened set (143 fixtures today, exactly what MIN_FIXTURES pins), and the walk
# below inventories that set's assertion blocks the way
# `declare_assertion_block` inventories them at load time.
#
# Deliberately NOT derived from the manifest and not from what the run read: an
# expectation taken from the run moves WITH the run, so a loader that detaches
# takes the expectation to 0 alongside it and this rung reports "0 == 0, OK".
#
# TWO dimensions, both from the ONE walk below, both asserted EQUAL
# (#lzblocksitepin) — never `>=`, because every drift this rung exists to report
# arrives as corpus GROWTH first and a floor cannot see growth:
#
#   * SITES, `fixture|where` pairs. A site is lost when a block is deleted even
#     if its bytes recur at another site.
#   * DISTINCT DIGESTS. A digest is lost when a content edit collapses two
#     distinct claims into one spelling, which leaves every site in place.
#
# 710 sites carry 607 distinct digests today, so 103 sites share their bytes with
# another site and 52 digests recur. The two dimensions are therefore genuinely
# independent here, which they were NOT under the narrow walk: at 15 and 15 no
# two blocks were spelled alike, and the deletion form of the site probe did not
# exist because no digest recurred.
#
# SCOPE: this walk reads all five block names at EVERY depth, object-valued only
# (#lzcppblockwalk). It replaced one that read the TOP-LEVEL `assertions` object
# and nothing else — 15 of 710 sites, with 128 of the 143 opened fixtures
# carrying no top-level `assertions` at all, so rung 0 reported nothing
# whatsoever about them. 710/607 is also the five-name object-valued row of
# lazily-spec's `make corpus-blocks-report` for this binding's opened set, which
# is corroboration and not the source: the expectation is derived from the rule
# THIS repo implements.
if ! KNOWN_UNCOVERED_LEDGER="$(printf '%s\n' ${KNOWN_UNCOVERED[@]+"${KNOWN_UNCOVERED[@]}"})" \
     KNOWN_UNBOUND_LEDGER="$(printf '%s\n' ${KNOWN_UNBOUND_BLOCKS[@]+"${KNOWN_UNBOUND_BLOCKS[@]}"})" \
     python3 - "$manifest" "$conformance_dir" <<'BLOCK_MAGNITUDE'
import json
import os
import sys

manifest_path, spec_dir = sys.argv[1], sys.argv[2]

# ---- what the RUN inventoried -------------------------------------------
declared_sites = {}
bound_digests = set()
with open(manifest_path, encoding="utf-8") as handle:
    for line in handle:
        parts = line.rstrip("\n").split("\t")
        if parts[0] == "@block_declared" and len(parts) == 3:
            declared_sites[parts[1]] = parts[2]
        elif parts[0] == "@block_bound" and len(parts) == 2:
            bound_digests.add(parts[1])
declared_digests = set(declared_sites.values())

# "fixture|where" -> reason, for sites nothing can bind. Split on the FIRST two
# pipes only: a reason is prose and may contain one.
unbound_excuses = {}
for entry in os.environ.get("KNOWN_UNBOUND_LEDGER", "").splitlines():
    entry = entry.strip()
    if not entry:
        continue
    parts = entry.split("|", 2)
    if len(parts) != 3 or not parts[2].strip():
        print(
            "ERROR: KNOWN_UNBOUND_BLOCKS entry %r is not \"fixture|where|reason\" with a\n"
            "       non-empty reason. An excuse with no reason is an unexplained gap\n"
            "       wearing a guard's uniform." % (entry,),
            file=sys.stderr,
        )
        sys.exit(1)
    unbound_excuses["%s|%s" % (parts[0].strip(), parts[1].strip())] = parts[2].strip()

# ---- the twin of the loader's walk and digest ---------------------------
#
# `walk_assertion_blocks` (tests/test_assertion_keys.hpp), clause for clause:
# every name in {assertions, expect, expect_after, expect_initial, expected}, at
# EVERY depth, OBJECT-valued only; an ARRAY-valued tracked key contributes NO
# site (a runner binds the elements, never the array, so counting the array
# would declare a block unbindable by construction) though arrays are still
# descended into, which is where `steps[3].expect` lives; and a block is EMITTED
# AND NOT DESCENDED INTO, because descending would inventory a fixture's
# `expect` nested inside its own `assertions` as a second, separately bindable
# site no tracker can reach without unwrapping the first. `where` is spelled
# from the loader's coordinates: dotted member names, `[n]` for array indices.
#
# `write_canonical` + `assertion_block_digest`, rule for rule: objects emit
# `{` then each member as `<name>:<value>,` with names SORTED, arrays emit `[`
# then each element as `<value>,`, strings emit `"<decoded text>"`, numbers emit
# `#<RAW LEXICAL TOKEN>` (never a reparsed value — normalising `1.0` to `1`
# would split one block into two), booleans `t`/`f`, null `n`; the result is
# folded with 64-bit FNV-1a and printed as 16 hex digits.
#
# ONE walk, TWO callers: `walk_blocks` collects the site AND the block value, so
# the site expectation and the digest expectation come from the same traversal.
# A digest expectation derived by a second walk could disagree with the site one
# and neither would be wrong about its own rule.
FNV_OFFSET = 0xCBF29CE484222325
FNV_PRIME = 0x100000001B3
MASK = (1 << 64) - 1


class RawNumber:
    """A JSON number kept as the exact token the file carries."""

    __slots__ = ("raw",)

    def __init__(self, raw):
        self.raw = raw


def write_canonical(value, out):
    if isinstance(value, dict):
        out.append("{")
        for name in sorted(value):
            out.append(name + ":")
            write_canonical(value[name], out)
            out.append(",")
        out.append("}")
    elif isinstance(value, list):
        out.append("[")
        for item in value:
            write_canonical(item, out)
            out.append(",")
        out.append("]")
    elif isinstance(value, str):
        out.append('"' + value + '"')
    elif isinstance(value, RawNumber):
        out.append("#" + value.raw)
    elif value is True:
        out.append("t")
    elif value is False:
        out.append("f")
    elif value is None:
        out.append("n")
    else:
        raise TypeError("unexpected JSON node %r" % (value,))
    return out


def block_digest(block):
    hash_value = FNV_OFFSET
    for byte in "".join(write_canonical(block, [])).encode("utf-8"):
        hash_value ^= byte
        hash_value = (hash_value * FNV_PRIME) & MASK
    return "%016x" % hash_value


ASSERTION_BLOCK_NAMES = ("assertions", "expect", "expect_after", "expect_initial", "expected")


def walk_blocks(fixture_id, node, path, sites, blocks):
    if isinstance(node, dict):
        for name, value in node.items():
            child = name if not path else path + "." + name
            if name in ASSERTION_BLOCK_NAMES and isinstance(value, dict):
                sites[fixture_id + "|" + child] = value
                blocks.append(value)
                continue
            walk_blocks(fixture_id, value, child, sites, blocks)
    elif isinstance(node, list):
        for index, item in enumerate(node):
            walk_blocks(fixture_id, item, "%s[%d]" % (path, index), sites, blocks)


# ---- the corpus listing, minus this binding's own ledger ----------------
corpus_fixtures = []
for walk_root, _walk_dirs, walk_names in os.walk(spec_dir):
    for walk_name in walk_names:
        if walk_name.endswith(".json"):
            corpus_fixtures.append(
                os.path.relpath(os.path.join(walk_root, walk_name), spec_dir).replace(os.sep, "/")
            )
corpus_fixtures.sort()

# The ledger arrives as a newline-joined scalar rather than as argv so this
# script adds no second top-level bash array for lazily-spec's
# check-corpus-floors.mjs to have to classify.
uncovered_ledger = {
    entry.strip()
    for entry in os.environ.get("KNOWN_UNCOVERED_LEDGER", "").splitlines()
    if entry.strip()
}

expected_sites = {}
expected_blocks = []
walked = 0
for fixture_id in corpus_fixtures:
    if fixture_id in uncovered_ledger:
        continue
    try:
        with open(os.path.join(spec_dir, fixture_id), encoding="utf-8") as handle:
            # `parse_int`/`parse_float` keep the RAW token, because that is what
            # `write_canonical` folds.
            document = json.load(handle, parse_int=RawNumber, parse_float=RawNumber)
    except (OSError, ValueError) as error:
        print(
            "ERROR: could not read canonical fixture '%s' out of %s: %s\n"
            "       The expected block magnitude is derived from these bytes, so an\n"
            "       unreadable fixture is missing EVIDENCE, not evidence of absence."
            % (fixture_id, spec_dir, error),
            file=sys.stderr,
        )
        sys.exit(1)
    walk_blocks(fixture_id, document, "", expected_sites, expected_blocks)
    walked += 1

expected_digests = {block_digest(block) for block in expected_blocks}

# Positive-evidence guard, ONE ARM PER DIMENSION (#lzvacuousrun): an empty
# derivation is matched by a run that inventoried nothing, and zero == zero
# reports OK having compared nothing. A derived expectation of zero is a hard
# error, not a satisfied one.
#
# The two arms are written separately even though one walk feeds both, so that
# neither dimension's zero-guard can be deleted while the other keeps the rung
# looking guarded. Both were verified by removing one arm at a time and watching
# the other still refuse an emptied corpus.
if walked == 0:
    print(
        "ERROR: the corpus at %s minus KNOWN_UNCOVERED derived ZERO opened fixtures.\n"
        "       Every number below is derived from those bytes, so this rung would be\n"
        "       vacuously green. The checkout is wrong, or LAZILY_SPEC_CONFORMANCE_DIR\n"
        "       points somewhere else." % (spec_dir,),
        file=sys.stderr,
    )
    sys.exit(1)

if not expected_sites:
    print(
        "ERROR: the corpus at %s minus KNOWN_UNCOVERED derived ZERO assertion-block\n"
        "       SITES over %d opened fixture(s). Zero expected sites are trivially\n"
        "       matched by a run that inventoried nothing, so this dimension would\n"
        "       report OK having compared nothing." % (spec_dir, walked),
        file=sys.stderr,
    )
    sys.exit(1)

if not expected_digests:
    print(
        "ERROR: the corpus at %s minus KNOWN_UNCOVERED derived ZERO distinct\n"
        "       assertion-block DIGESTS over %d opened fixture(s). Same vacuity as the\n"
        "       site arm above, on the dimension the site count cannot see."
        % (spec_dir, walked),
        file=sys.stderr,
    )
    sys.exit(1)

if not declared_sites:
    print(
        "ERROR: the run exported NO assertion-block inventory, but the corpus at %s\n"
        "       minus KNOWN_UNCOVERED derives %d site(s) over %d opened fixtures.\n"
        "       `declare_assertion_block` is no longer reached from `spec_fixture_text`,\n"
        "       or the manifest flush lost its @block_declared lines. Rung 0's bind\n"
        "       guard is SILENT in this state — an empty ledger has no unbound block."
        % (spec_dir, len(expected_sites), walked),
        file=sys.stderr,
    )
    sys.exit(1)

failed = False

if len(declared_sites) != len(expected_sites):
    direction = "FEWER than" if len(declared_sites) < len(expected_sites) else "MORE than"
    print(
        "ERROR: the run inventoried %d assertion-block SITES; the canonical corpus at\n"
        "       %s minus KNOWN_UNCOVERED derives %d over %d opened fixtures.\n"
        "       The run has %s the corpus declares.\n"
        "       This is an EQUALITY, not a floor: either the corpus moved under this\n"
        "       checkout (re-pull the lazily-spec sibling so both sides read the same\n"
        "       bytes), or `declare_assertion_block` detached from the rule the walk\n"
        "       above mirrors. There is no number to re-pin — fix whichever side moved."
        % (len(declared_sites), spec_dir, len(expected_sites), walked, direction),
        file=sys.stderr,
    )
    failed = True

if declared_digests != expected_digests:
    direction = (
        "FEWER than" if len(declared_digests) < len(expected_digests) else "MORE than"
    )
    if len(declared_digests) == len(expected_digests):
        direction = "the same count as, but a different set from,"
    print(
        "ERROR: the run inventoried %d DISTINCT assertion-block digests; the canonical\n"
        "       corpus at %s minus KNOWN_UNCOVERED derives %d over %d opened\n"
        "       fixtures. The run has %s the corpus declares.\n"
        "       The SITE count above can agree while this does not: two sites spelled\n"
        "       identically share one digest, so a content edit that collapses two\n"
        "       distinct claims into one leaves the site count untouched.\n"
        "       Either the corpus moved under this checkout, or `write_canonical` /\n"
        "       `assertion_block_digest` and the twin in this script stopped agreeing.\n"
        "       run-only: %s\n"
        "       corpus-only: %s"
        % (
            len(declared_digests),
            spec_dir,
            len(expected_digests),
            walked,
            direction,
            ",".join(sorted(declared_digests - expected_digests)) or "(none)",
            ",".join(sorted(expected_digests - declared_digests)) or "(none)",
        ),
        file=sys.stderr,
    )
    failed = True

# ---- the BIND half (#lznullformblind), judged here ----------------------
#
# This is the whole verdict now, not a second opinion: tests/test_assertion_keys.hpp
# used to abort in-process on any declared-but-unbound block, which was right
# while the walk read only the 15 top-level `assertions` objects (all bound, so
# nothing to excuse) and is wrong over 710 sites, 25 of which are genuinely
# unreachable. The per-site excuse ledger has to live in ONE place, and this is
# the place the rest of this binding's ledgers live.
unbound = sorted(site for site, digest in declared_sites.items() if digest not in bound_digests)
unexcused = [site for site in unbound if site not in unbound_excuses]
if unexcused:
    print(
        "ERROR: %d declared assertion-block site(s) were never BOUND to an AssertionKeys:\n"
        "       %s\n"
        "       Every other rung is scoped to a bound block, so an unbound one is not\n"
        "       reported as unread — it is not reported at all (#lznullformblind).\n"
        "       Bind it, or add it to KNOWN_UNBOUND_BLOCKS with the reason it cannot be."
        % (len(unexcused), "\n       ".join(unexcused)),
        file=sys.stderr,
    )
    failed = True

# Both directions, exactly as KNOWN_UNCOVERED is checked. An excuse is only
# honest while the site is still DECLARED by the corpus AND still UNBOUND.
unbound_set = set(unbound)
for site in sorted(unbound_excuses):
    if site not in declared_sites:
        print(
            "ERROR: KNOWN_UNBOUND_BLOCKS excuses '%s', which no opened fixture declares.\n"
            "       The block was deleted or moved upstream, or the walk stopped reaching\n"
            "       it — either way the excuse now hides nothing. Delete it."
            % (site,),
            file=sys.stderr,
        )
        failed = True
    elif site not in unbound_set:
        print(
            "ERROR: KNOWN_UNBOUND_BLOCKS excuses '%s', but this run DID bind it — the\n"
            "       excuse is stale and now understates coverage. Delete it."
            % (site,),
            file=sys.stderr,
        )
        failed = True

# A CEILING on the excused population, and the resolution of a disagreement
# between lazily-rs and lazily-kt over whether a typed count belongs beside a
# set equality (#lzledgerceiling, carried from #lzrsbindpending).
#
# A typed count that MIRRORS the current population is redundant with the
# equality above and can only ever drift away from it: if the ledger set and the
# unbound set are equal then their counts are equal, so the number carries no
# information the equality does not, and it adds a second edit site. That is the
# `MIN_BLOCKS = 30` shape, and refusing it is right. This binding never had one —
# every 710/607/25 in this file is prose, and the two dimensions above are
# DERIVED from the corpus listing rather than typed.
#
# But set equality alone has a hole that a bare count does close: it is satisfied
# by ANY CONSISTENT PAIR. A commit that detaches N binds AND writes the N
# matching entries passes both directions. Nothing above sees it — the magnitude
# rung does not either, because those sites are still DECLARED, they have merely
# stopped being BOUND, so the site count and the digest set are untouched.
#
# So the missing guard is not a count of what IS excused; it is a ceiling on how
# much MAY be. A ceiling is POLICY rather than MEASUREMENT: it does not move with
# the corpus and never needs re-pinning except deliberately and upward, in
# review. What it buys is that a regression and its excuse can no longer land in
# the same commit unnoticed — raising this line is the explicit act.
#
# It defaults to the population this binding carries today (25), so landing it is
# a no-op and any GROWTH fails. Raise it ONLY for a genuinely unbindable block —
# a step past a replay stop, with the reason the capability cannot exist — and
# expect to be asked why. Never to park a block a runner could bind.
#
# A ledger may only SHRINK. When the missing op lands and an entry goes stale,
# lower this line by the same amount in the same commit.
MAX_LEDGERED_BLOCKS = int(os.environ.get("MAX_LEDGERED_BLOCKS", "25"))
if len(unbound_excuses) > MAX_LEDGERED_BLOCKS:
    print(
        "ERROR: %d assertion-block site(s) are ledgered in KNOWN_UNBOUND_BLOCKS as\n"
        "       unbindable; the ceiling MAX_LEDGERED_BLOCKS is %d. This ledger may only\n"
        "       SHRINK.\n"
        "       The equality above only checks that the ledger and the run AGREE, which\n"
        "       any consistent pair satisfies — a commit that detaches binds and writes\n"
        "       the matching entries passes both of its directions, and the magnitude\n"
        "       rung misses it too because those sites are still DECLARED, merely no\n"
        "       longer BOUND. This ceiling is what makes enlarging the excused set an\n"
        "       explicit act instead of a side effect.\n"
        "       Bind the block. Raise this line only for a genuinely unbindable one,\n"
        "       with the reason the capability cannot exist:"
        % (len(unbound_excuses), MAX_LEDGERED_BLOCKS),
        file=sys.stderr,
    )
    for site in sorted(unbound_excuses):
        print("         %s | %s" % (site, unbound_excuses[site]), file=sys.stderr)
    failed = True

if failed:
    sys.exit(1)

print(
    "assertion-block magnitude OK: the run inventoried %d site(s) / %d distinct digest(s); "
    "%d bound, %d declared unbindable with a reason of at most %d; derived %d AND %d from "
    "the %d opened fixtures of the corpus listing minus KNOWN_UNCOVERED, both asserted "
    "EQUAL (all five block names, every depth, object-valued only), the ledger under a "
    "CEILING that makes enlarging it an explicit act"
    % (
        len(declared_sites),
        len(declared_digests),
        len(declared_sites) - len(unbound),
        len(unbound),
        MAX_LEDGERED_BLOCKS,
        len(expected_sites),
        len(expected_digests),
        walked,
    )
)
BLOCK_MAGNITUDE
then
  exit 1
fi

echo "conformance coverage OK: $count canonical fixtures replayed across ${#REQUIRED_AREAS[@]} areas" \
     "(${#EXCUSED_AREAS[@]} areas excused, ${#KNOWN_UNCOVERED[@]} fixtures listed as known-uncovered)"
echo "scenario coverage OK: $scenario_count of $(wc -l < "$declared_scenarios" | tr -d ' ')" \
     "declared scenarios replayed (${#SCENARIO_EXCUSES[@]} excused)"
