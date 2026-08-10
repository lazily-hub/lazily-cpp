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

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
manifest="${1:-$repo_root/build/conformance-fixtures-loaded.txt}"
conformance_dir="${LAZILY_SPEC_CONFORMANCE_DIR:-$repo_root/../lazily-spec/conformance}"

# Absence of the sibling checkout is a clean SKIP, consistent with the suites
# themselves (CTest SKIP_RETURN_CODE 77 via require_spec_checkout_or_skip). It is
# NOT a pass: nothing was verified and the message says so.
if [[ ! -d "$conformance_dir" ]]; then
  echo "SKIP: canonical conformance corpus not found at '$conformance_dir'"
  echo "      clone the sibling checkout to run the conformance coverage guard:"
  echo "      git clone https://github.com/lazily-hub/lazily-spec.git ../lazily-spec"
  exit 0
fi

# Minimum total DISTINCT fixtures replayed. Raise this when replays are added;
# NEVER lower it to make a red build green — a drop means a replay stopped
# running.
#
# The current suite replays 127 distinct fixtures. Six collections fixtures
# (MergeCell, reconciliation, SemTree, stable IDs, and two TextCrdt corpora)
# moved out of KNOWN_UNCOVERED in #lazilycppcollections, the seven ingress
# schedules landed with the transport-agnostic ingress family,
# codec/frame_roundtrip_json.json plus distributed/crdt_sync_frames.json joined
# them with the json reference codec (#lzcppjsoncodec, #lazilycppexcuses), and
# codec/frame_roundtrip_msgpack.json joined them with the spec `msgpack` wire
# (#lzcppmsgpackwire), and codec/nodeid_exact_range.json joined them with the
# NodeId exact-representation bound (#lzspecdecoderbound) — the runner that found
# `uint 64` wrapping to a negative int64_t in read_i64(), and
# codec/nodekey_null_leniency.json joined them with the NodeKey null-leniency
# rule (#lzkeynullstrict), and codec/blob_backend_discriminator.json joined them
# with the blob-backend discriminator clause (#lzblobbackendstrict) — the runner
# that holds an unknown `backend` token REFUSED rather than normalized to `shm`;
# keep the floor aligned so deleting a runner cannot hide behind older aggregate
# growth.
MIN_FIXTURES="${MIN_FIXTURES:-127}"

# Areas lazily-cpp is expected to replay. An area belongs here once a runner
# opens its fixtures through `spec_fixture_text`; listing an area the binding
# does not read at all would make this guard permanently red, and omitting one it
# does read would let that runner vanish unnoticed — which is the whole point.
#
# Deliberately ABSENT, with reasons (open gaps, not oversights). `codec` left
# this list in #lzcppjsoncodec and is now fully replayed: json_codec.hpp is the
# `json` REFERENCE codec and msgpack_codec.hpp is the `msgpack` CROSS-LANGUAGE
# BINARY DEFAULT (#lzcppmsgpackwire), so both halves of the frame-codec
# obligation go THROUGH a real codec and neither is named in KNOWN_UNCOVERED:
#   signaling  (1)  anti_spoof_session.json IS replayed by
#                   test_signaling_conformance.cpp. frames.json is not: it needs
#                   signaling wire serde, which this binding does not have.
#   agent-doc  (2)  IPC wire snapshots of the agent-doc state projection — an
#                   application schema carried on the IPC plane, not a
#                   binding-level reactive concern.
#   ipc        (1)  arena_blob.json locks the concrete 40-byte shared-memory
#                   arena host layout. C++ implements an in-process arena with
#                   the same descriptor semantics but not that mapped header.
#                   The seven Snapshot/Delta fixtures are replayed by
#                   test_ipc.cpp.
REQUIRED_AREAS=(
codec
collections
coordination
crdt-tree
distributed
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
  resilience
  service
  statechart
  stdlib
  temporal
  windowing
)

# Fixtures inside a REQUIRED area that this binding does NOT yet replay, each
# with the reason it is outstanding. This list is the honest ledger of the gap:
# a fixture is either replayed or named here, never silently absent. Entries are
# verified BOTH to still exist on disk and to still be unread, so neither a
# fixture renamed/deleted upstream nor one this suite started replaying can rot
# into a permanent excuse.
#
# Shrinking this list is the work. Growing it requires a stated reason.
KNOWN_UNCOVERED=(
  # Register CRDTs (LWW / MV / PnCounter + the CellCrdt projection bit) are
  # implemented here, but this binding has no canonical replay for the new
  # registers corpus yet; the Registers coverage row is `~` until it does.
  "collections/registers_convergence.json"
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
# MORE than 144 and still passes — the check is a minimum. Raise this once the
# scenarios land upstream, never to match a working tree.
#
# Raise this when replays are added; NEVER lower it to make a red build green.
MIN_SCENARIOS="${MIN_SCENARIOS:-144}"

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

# Completeness: every canonical fixture in a required area is replayed, or named
# in KNOWN_UNCOVERED. This is the check that catches the corpus GROWING.
for area in "${REQUIRED_AREAS[@]}"; do
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

echo "conformance coverage OK: $count canonical fixtures replayed across ${#REQUIRED_AREAS[@]} areas" \
     "(${#KNOWN_UNCOVERED[@]} fixtures listed as known-uncovered)"
echo "scenario coverage OK: $scenario_count of $(wc -l < "$declared_scenarios" | tr -d ' ')" \
     "declared scenarios replayed (${#SCENARIO_EXCUSES[@]} excused)"
