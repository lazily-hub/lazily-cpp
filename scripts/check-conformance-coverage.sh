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
# positional spelling; those are REPORTED rather than silently accepted.
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
# The current suite replays 112 distinct fixtures. Six collections fixtures
# (MergeCell, reconciliation, SemTree, stable IDs, and two TextCrdt corpora)
# moved out of KNOWN_UNCOVERED in #lazilycppcollections, and the seven ingress
# schedules landed with the transport-agnostic ingress family; keep the floor
# aligned so deleting a runner cannot hide behind older aggregate growth.
MIN_FIXTURES="${MIN_FIXTURES:-112}"

# Areas lazily-cpp is expected to replay. An area belongs here once a runner
# opens its fixtures through `spec_fixture_text`; listing an area the binding
# does not read at all would make this guard permanently red, and omitting one it
# does read would let that runner vanish unnoticed — which is the whole point.
#
# Deliberately ABSENT, with reasons (open gaps, not oversights):
#   signaling  (1)  anti_spoof_session.json IS replayed by
#                   test_signaling_conformance.cpp. frames.json is not: it needs
#                   signaling wire serde, which this binding does not have.
#   familysync (1)  test_family_sync.cpp mirrors materialize_on_ingest.json by
#                   hand; it never opens the file.
#   agent-doc  (2)  IPC wire snapshots of the agent-doc state projection — an
#                   application schema carried on the IPC plane, not a
#                   binding-level reactive concern.
#   root-level (8)  snapshot_*/delta_*/arena_blob IPC wire fixtures; test_ipc.cpp
#                   hand-builds the equivalent structures.
REQUIRED_AREAS=(
  collections
  coordination
  crdt-tree
  distributed
  ingress
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
  # distributed — CrdtSync wire-serde frames; lazily-cpp has no JSON codec for
  # the IpcMessage envelope, so replaying `wire` needs an encoder first.
  "distributed/crdt_sync_frames.json"
  # materialization — mirrored by hand in test_reactive_family.cpp and its
  # async / thread-safe siblings; only entry_kind_orthogonal_to_mode.json is
  # replayed from the canonical bytes.
  "materialization/deferral_not_deallocation.json"
  "materialization/observational_transparency.json"
  # reliable-sync — test_reliable_sync.cpp transcribes five of these by hand and
  # two have no coverage in any form.
  "reliable-sync/coalesce_bounds_outbox.json"
  "reliable-sync/idempotent_redelivery.json"
  "reliable-sync/liveness_lease_eviction.json"
  "reliable-sync/liveness_orset_lww.json"
  "reliable-sync/multi_epoch_delta.json"
  "reliable-sync/outbox_replay_after_crash.json"
  "reliable-sync/resync_gap_converge.json"
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

# reliable-sync/outbox_store_protocol.json — tests/test_outbox_store.cpp opens
# the fixture only to assert the four scenario names still exist upstream, then
# drives the store from hand-written constants. A transcription is not a replay:
# every value it checks is one somebody typed, so the fixture cannot disprove
# it. Converting the runner is tracked as its own item; excused here rather than
# recorded as replayed, because recording it would be the false claim.
excuse_scenario "reliable-sync/outbox_store_protocol.json" \
  "unordered puts replay in ascending epoch order" \
  "hand-transcribed in test_outbox_store.cpp; the runner greps the fixture text instead of replaying its steps"
excuse_scenario "reliable-sync/outbox_store_protocol.json" \
  "ack cursor is monotone and prune-safe" \
  "hand-transcribed in test_outbox_store.cpp; the runner greps the fixture text instead of replaying its steps"
excuse_scenario "reliable-sync/outbox_store_protocol.json" \
  "restart reloads cursor and unacked suffix" \
  "hand-transcribed in test_outbox_store.cpp; the runner greps the fixture text instead of replaying its steps"
excuse_scenario "reliable-sync/outbox_store_protocol.json" \
  "stale handle cannot regress serialized cursor" \
  "hand-transcribed in test_outbox_store.cpp; the runner greps the fixture text instead of replaying its steps"

# Minimum DISTINCT scenarios replayed, the per-scenario twin of MIN_FIXTURES.
# Without it a ledger that stopped recording entirely would leave the
# declared-vs-replayed comparison vacuously satisfied on both sides.
MIN_SCENARIOS="${MIN_SCENARIOS:-75}"

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
positional_scenarios="$(mktemp)"
trap 'rm -f "$replayed" "$declared_scenarios" "$replayed_scenarios" "$positional_scenarios"' EXIT
sort -u "$manifest" | grep . | grep -v '^@' > "$replayed" || true
count="$(wc -l < "$replayed" | tr -d ' ')"

# Scenario records are tab-delimited and tag-prefixed (tests/test_spec_fixture.hpp).
# `declared` is what the corpus on disk carries for every fixture this run
# opened; `replayed` is what a runner actually entered.
awk -F'\t' '$1=="@declared"   {print $2 "\t" $3}' "$manifest" | sort -u > "$declared_scenarios"
awk -F'\t' '$1=="@replayed"   {print $2 "\t" $3}' "$manifest" | sort -u > "$replayed_scenarios"
awk -F'\t' '$1=="@positional" {print $2 "\t" $3}' "$manifest" | sort -u > "$positional_scenarios"
scenario_count="$(wc -l < "$replayed_scenarios" | tr -d ' ')"

status=0

if (( count < MIN_FIXTURES )); then
  echo "ERROR: only $count distinct conformance fixtures replayed, expected >= $MIN_FIXTURES." >&2
  echo "       A replay was removed, renamed, or short-circuited. Do not lower MIN_FIXTURES to fix this." >&2
  status=1
fi

for area in "${REQUIRED_AREAS[@]}"; do
  if ! grep -q "^${area}/" "$replayed"; then
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
  [[ -d "$conformance_dir/$area" ]] || continue
  while IFS= read -r fixture; do
    [[ -n "$fixture" ]] || continue
    id="${area}/${fixture}"
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
  done < <(cd "$conformance_dir/$area" && ls -1 *.json 2>/dev/null || true)
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

# Reported, never silently accepted: an id resolved by position means the corpus
# gives that scenario no `id` and no `name`, so the ledger cannot survive a
# reordering upstream. Visibility here is what makes the corpus gap fixable.
if [[ -s "$positional_scenarios" ]]; then
  echo "NOTE: $(wc -l < "$positional_scenarios" | tr -d ' ') scenario(s) carry no 'id' and no 'name'" \
       "upstream and are ledgered by position:"
  while IFS= read -r pair; do
    [[ -n "$pair" ]] || continue
    echo "        ${pair%%$'\t'*} scenario ${pair#*$'\t'}"
  done < "$positional_scenarios"
fi

echo "conformance coverage OK: $count canonical fixtures replayed across ${#REQUIRED_AREAS[@]} areas" \
     "(${#KNOWN_UNCOVERED[@]} fixtures listed as known-uncovered)"
echo "scenario coverage OK: $scenario_count of $(wc -l < "$declared_scenarios" | tr -d ' ')" \
     "declared scenarios replayed (${#SCENARIO_EXCUSES[@]} excused)"
