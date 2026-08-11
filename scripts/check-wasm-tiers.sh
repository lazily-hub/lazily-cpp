#!/usr/bin/env bash
# Generates AND audits the wasm capability matrix in WASM.md (`#lzcppwasm` Phase 3).
#
# The matrix is derived from the runtime fixture manifests the wasm runs
# actually produced — never hand-authored. That distinction is the whole point.
# lazily-cpp's own queue tests were once hand-transcribed rather than replayed,
# and a hand-written table is the same defect one level up: it describes what
# somebody believed rather than what ran, and nothing makes it wrong when the
# belief expires.
#
# Inputs (all produced by `make wasm-core` / `make wasm-threaded`):
#   build_wasm_core/conformance-fixtures-loaded.txt      core tier manifest
#   build_wasm_threaded/conformance-fixtures-loaded.txt  threaded tier manifest
#   wasm-tiers.conf                                      tier assignment
#   ../lazily-spec/conformance/                          the canonical corpus
#
# Failures detected:
#   1. a tier manifest is missing or below its floor — that tier stopped running
#   2. a tier built runners the conf does not assign to it, or vice versa
#   3. a canonical family with NO wasm replay and no reason in
#      WASM_ABSENT_REASONS — a silent gap, which is how a matrix starts lying
#   4. a WASM_ABSENT_REASONS entry for a family that IS replayed on wasm — a
#      stale excuse that understates the target (checked in both directions,
#      exactly as KNOWN_UNCOVERED is in check-conformance-coverage.sh)
#   5. a WASM_ABSENT_REASONS entry naming a family the corpus does not contain
#   6. the committed WASM.md matrix differs from the generated one — the table
#      was edited by hand, or a run changed coverage and nobody regenerated
#
# Usage:
#   scripts/check-wasm-tiers.sh            audit; fail on any drift
#   scripts/check-wasm-tiers.sh --write    regenerate WASM.md, then audit

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"

# Corpus counting, row rendering, the markers, and LC_ALL=C live in the shared
# library so this generator and the toolchain-free rung in `make check`
# (scripts/check-wasm-corpus-column.sh) cannot disagree about what a Corpus cell
# says (`#lzcppwasmguardlocal`).
# shellcheck source=lib/wasm-matrix.sh
source "$repo_root/scripts/lib/wasm-matrix.sh"

conformance_dir="$(wasm_corpus_dir "$repo_root")"
tier_conf="$repo_root/wasm-tiers.conf"
wasm_md="$repo_root/WASM.md"

core_manifest="${CORE_MANIFEST:-$repo_root/build_wasm_core/conformance-fixtures-loaded.txt}"
threaded_manifest="${THREADED_MANIFEST:-$repo_root/build_wasm_threaded/conformance-fixtures-loaded.txt}"

write_mode=0
[[ "${1:-}" == "--write" ]] && write_mode=1

# Floors. Raise when replays are added; NEVER lower one to make a red build
# green — a drop means a replay stopped running. These are the positive
# assertion that fixtures ran, without which "0 replayed" reads as success.
MIN_CORE_FIXTURES="${MIN_CORE_FIXTURES:-86}"
MIN_THREADED_FIXTURES="${MIN_THREADED_FIXTURES:-44}"

# ── the one narrowing ledger ────────────────────────────────────────────────
#
# A canonical family with no replay on any wasm tier MUST appear here with a
# reason. This is the only mechanism that excuses a family, deliberately: a
# guard that reads one ledger and silently ignores a second one does not weaken
# the audit, it inverts it for whoever added the second (`#lzgdcoveragecolumn`).
# If a narrowing mechanism is ever added, it goes in this array or the guard
# must be taught about it explicitly.
#
# Format: "family|reason"
# Two distinct kinds of absence live here, and the reason text says which:
# a family wasm CANNOT carry, and a family lazily-cpp does not replay at ALL
# (native included). Collapsing them would let "we never wrote this runner" hide
# behind "wasm can't do this" — the difference between has-not and cannot, the
# same distinction `⊘` preserves in coverage.json.
WASM_ABSENT_REASONS=(
  "reliable-sync|the durable outbox is file-backed (unistd.h/fcntl.h); reliable_sync.hpp refuses to compile under __EMSCRIPTEN__"
  "distributed|resolves ShmBlobRef through transport.hpp's ShmBackend — POSIX shm_open/mmap has no wasm implementation"
  "(root)|the top-level snapshot_*/delta_* frames are replayed by the ipc and reliable-sync suites, both native-only"
  "egress|lazily-cpp has no egress runner in ANY target, native included — a binding gap, not a wasm limit"
  "protobuf|lazily-cpp ships no protobuf codec in ANY target, native included — a binding gap, not a wasm limit"
  "agent-doc|agent-doc session fixtures are not a lazily library family and no binding replays them"
)

status=0
fail() { echo "ERROR: $*" >&2; status=1; }

# ── read the manifests ──────────────────────────────────────────────────────
#
# Manifest lines beginning with @ are the per-scenario ledger; the fixture-level
# lines are the rest. Counting the @ lines as fixtures would inflate every cell.
# The `|| true` is load-bearing. Under `set -euo pipefail`, `grep` exits 1 when
# it selects no lines, so on an EMPTY manifest this function would abort the
# whole script before the floor check below could report why — the guard would
# fail closed but silently, and "exit 1 with no message" is indistinguishable
# from a crash. The empty case is precisely the one that needs a diagnostic.
fixtures_of() {
  local manifest="$1"
  [[ -f "$manifest" ]] || return 0
  { grep -v '^@' "$manifest" || true; } | sed '/^$/d' | sort -u
}

require_manifest() {
  local manifest="$1" label="$2" floor="$3"
  if [[ ! -f "$manifest" ]]; then
    fail "$label manifest not found at '$manifest'. Run 'make wasm-$label' first — an absent manifest is not an empty tier, it is an unrun one."
    return
  fi
  local n
  n="$(fixtures_of "$manifest" | wc -l)"
  if (( n < floor )); then
    fail "$label tier replayed $n distinct fixtures, below the floor of $floor. A drop means a replay stopped running; do not lower the floor to go green."
  fi
}

require_manifest "$core_manifest" core "$MIN_CORE_FIXTURES"
require_manifest "$threaded_manifest" threaded "$MIN_THREADED_FIXTURES"

if [[ ! -d "$conformance_dir" ]]; then
  echo "SKIP: canonical conformance corpus not found at '$conformance_dir'"
  echo "      git clone https://github.com/lazily-hub/lazily-spec.git ../lazily-spec"
  exit 0
fi

# ── the conf agrees with what was actually built ────────────────────────────
#
# Both halves. A runner in the conf that the tier did not build would be
# reported as covered while contributing nothing; a runner the tier built that
# the conf does not list would be invisible in the matrix.
check_tier_targets() {
  local tier="$1" build_dir="$2"
  [[ -d "$build_dir" ]] || return 0
  local declared built
  declared="$(awk -F'|' -v t="$tier" '!/^[[:space:]]*#/ && NF>=3 && $1==t {print $2}' "$tier_conf" | sort -u)"
  built="$(find "$build_dir/tests" -maxdepth 1 -name 'test_*.js' -printf '%f\n' 2>/dev/null \
           | sed 's/\.js$//' | sort -u)"
  local only_declared only_built
  only_declared="$(comm -23 <(echo "$declared") <(echo "$built"))"
  only_built="$(comm -13 <(echo "$declared") <(echo "$built"))"
  if [[ -n "$only_declared" ]]; then
    fail "wasm-tiers.conf assigns these to tier '$tier' but the build produced no module for them: $(echo $only_declared)"
  fi
  if [[ -n "$only_built" ]]; then
    fail "tier '$tier' built these runners but wasm-tiers.conf does not list them, so the matrix cannot report them: $(echo $only_built)"
  fi
}
check_tier_targets core "$repo_root/build_wasm_core"
check_tier_targets threaded "$repo_root/build_wasm_threaded"

# ── per-family counts ───────────────────────────────────────────────────────
family_of() { [[ "$1" == */* ]] && echo "${1%%/*}" || echo "(root)"; }

declare -A CORE THREADED CORPUS
while read -r f; do [[ -n "$f" ]] && CORE["$(family_of "$f")"]=$(( ${CORE["$(family_of "$f")"]:-0} + 1 )); done < <(fixtures_of "$core_manifest")
while read -r f; do [[ -n "$f" ]] && THREADED["$(family_of "$f")"]=$(( ${THREADED["$(family_of "$f")"]:-0} + 1 )); done < <(fixtures_of "$threaded_manifest")

# Every family the canonical corpus carries, so a family nobody replays anywhere
# still appears as a row rather than vanishing from the table. Counted through
# the shared library — the same call the local corpus-column rung makes.
while IFS=$'\t' read -r d n; do
  CORPUS["$d"]="$n"
done < <(wasm_corpus_counts "$conformance_dir")

declare -A REASON
for entry in "${WASM_ABSENT_REASONS[@]}"; do
  fam="${entry%%|*}"; why="${entry#*|}"
  if [[ -z "${CORPUS[$fam]+x}" ]]; then
    fail "WASM_ABSENT_REASONS names family '$fam', which is not in the canonical corpus."
    continue
  fi
  REASON["$fam"]="$why"
done

# ── the two-directional audit ───────────────────────────────────────────────
#
# Deliberately stated against the CANONICAL CORPUS, not against a native
# manifest. The first version compared wasm coverage to what the native suite
# replayed, which made the audit depend on a manifest produced by a DIFFERENT
# CI job — so in the wasm job every native cell read zero and the whole table
# mismatched. Comparing to the corpus is both environment-independent and a
# stronger claim: every family the spec ships either replays on a wasm tier or
# carries a recorded reason, whatever the native suite happens to do.
for fam in "${!CORPUS[@]}"; do
  c=${CORE[$fam]:-0}; t=${THREADED[$fam]:-0}
  wasm=$(( c + t ))
  if (( wasm == 0 )) && [[ -z "${REASON[$fam]+x}" ]]; then
    fail "family '$fam' has ${CORPUS[$fam]} canonical fixture(s) and replays none on any wasm tier, and WASM_ABSENT_REASONS gives no reason. Either replay it on a tier or record why it cannot be — a blank cell is what makes a matrix unreadable."
  fi
  if (( wasm > 0 )) && [[ -n "${REASON[$fam]+x}" ]]; then
    fail "WASM_ABSENT_REASONS excuses family '$fam', but wasm replays $wasm fixture(s) of it — the excuse is stale and understates the target."
  fi
done

# ── generate the matrix ─────────────────────────────────────────────────────
generate_matrix() {
  echo "| Family | Corpus | wasm core | wasm threaded | Note |"
  echo "|---|---:|---:|---:|---|"
  local total_corpus=0 total_core=0 total_thr=0
  for fam in $(printf '%s\n' "${!CORPUS[@]}" | sort); do
    local corp=${CORPUS[$fam]:-0} c=${CORE[$fam]:-0} t=${THREADED[$fam]:-0}
    local note=""
    if [[ -n "${REASON[$fam]+x}" ]]; then
      note="not on wasm — ${REASON[$fam]}"
    elif (( c > 0 && t > 0 )); then
      note="split across both tiers"
    elif (( t > 0 )); then
      note="needs -pthread"
    fi
    # The family + Corpus cells come from the shared renderer, so the local rung
    # compares committed rows against the very bytes this generator emits.
    printf '%s %s | %s | %s |\n' "$(wasm_matrix_corpus_cells "$fam" "$corp")" \
      "$( ((c>0)) && echo "$c" || echo '—')" \
      "$( ((t>0)) && echo "$t" || echo '—')" "$note"
    total_corpus=$((total_corpus+corp)); total_core=$((total_core+c)); total_thr=$((total_thr+t))
  done
  printf '%s **%d** | **%d** | |\n' "$(wasm_matrix_total_corpus_cells "$total_corpus")" \
    "$total_core" "$total_thr"
}

start_marker="$WASM_MATRIX_START_MARKER"
end_marker="$WASM_MATRIX_END_MARKER"

if [[ ! -f "$wasm_md" ]]; then
  fail "WASM.md not found at '$wasm_md'"
  exit 1
fi
if ! grep -qF "$start_marker" "$wasm_md" || ! grep -qF "$end_marker" "$wasm_md"; then
  fail "WASM.md is missing the '$start_marker' / '$end_marker' markers the generator writes between."
  exit 1
fi

generated="$(generate_matrix)"
committed="$(wasm_matrix_committed_body "$wasm_md")"

if [[ "$write_mode" == "1" ]]; then
  tmp="$(mktemp)"
  awk -v s="$start_marker" -v e="$end_marker" -v body="$generated" \
    'index($0,s){print; print body; skip=1; next} index($0,e){skip=0} !skip' "$wasm_md" > "$tmp"
  mv "$tmp" "$wasm_md"
  echo "WASM.md: matrix regenerated from the runtime manifests."
elif [[ "$generated" != "$committed" ]]; then
  fail "the WASM.md matrix does not match the one generated from the runtime manifests. Run 'make wasm-matrix' and commit the result — do not edit the table by hand."
  diff <(echo "$committed") <(echo "$generated") | head -30 >&2 || true
fi

if (( status != 0 )); then
  echo "wasm tier/matrix guard: FAILED" >&2
  exit 1
fi

echo "wasm tier/matrix guard: OK ($(fixtures_of "$core_manifest" | wc -l) core + $(fixtures_of "$threaded_manifest" | wc -l) threaded distinct fixtures replayed)"
