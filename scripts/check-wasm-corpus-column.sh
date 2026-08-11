#!/usr/bin/env bash
# The toolchain-free half of the WASM.md matrix audit (`#lzcppwasmguardlocal`).
#
# WHY THIS EXISTS
#
#   The capability matrix in WASM.md was audited ONLY by scripts/check-wasm-tiers.sh,
#   which needs the emsdk runtime manifests and therefore runs only in the `wasm`
#   CI job. So a corpus change could pass `make check` and both native CI jobs on
#   a developer's machine and still land red on main. It did: 9b0ff08 added two
#   `lossless-tree` replays, went out locally clean, and reddened CI with
#
#     < | `lossless-tree` | 9 | 9 | — |  |
#     > | `lossless-tree` | 11 | 11 | — |  |
#
#   fixed after the fact in cbeedc6. The corpus is shared by nine bindings and
#   grows often, so that round trip is not a one-off.
#
# WHAT IT AUDITS
#
#   Exactly the part of the matrix that is a function of the corpus alone:
#
#     * the family ROW SET — every family ../lazily-spec/conformance carries has
#       a row, and every row names a family the corpus still carries
#     * the `Corpus` column — each row's count, and the **total**
#
#   No wasm build, no emsdk, no tier manifest participates in any of that.
#
# WHAT IT DOES NOT AUDIT — and says so on every run
#
#   The `wasm core` / `wasm threaded` columns, the per-tier notes, the
#   WASM_ABSENT_REASONS ledger, and the fixture floors. Those are only knowable
#   from a manifest an actual emscripten run produced, and they stay where they
#   are: scripts/check-wasm-tiers.sh in the `wasm` job. This rung does not weaken,
#   duplicate, or bypass that guard — it shares its counting and row-rendering
#   code (scripts/lib/wasm-matrix.sh) so the two cannot disagree about a cell.
#
# FAIL-CLOSED
#
#   A missing or unreadable corpus, a corpus with no fixtures, a WASM.md with no
#   markers, a table with no rows, or a row it cannot parse are all HARD FAILURES.
#   The failure this rung exists to catch is a count that moved; a run that
#   silently compares zero rows would report OK over exactly that.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"

# shellcheck source=lib/wasm-matrix.sh
source "$repo_root/scripts/lib/wasm-matrix.sh"

conformance_dir="$(wasm_corpus_dir "$repo_root")"
wasm_md="$repo_root/WASM.md"

status=0
fail() {
	echo "ERROR: $*" >&2
	status=1
}

# ── the corpus side ─────────────────────────────────────────────────────────
counts=""
if ! counts="$(wasm_corpus_counts "$conformance_dir")"; then
	echo "ERROR: canonical conformance corpus not found or not readable at '$conformance_dir'." >&2
	echo "       git clone https://github.com/lazily-hub/lazily-spec.git ../lazily-spec" >&2
	echo "       (or point LAZILY_SPEC_CONFORMANCE_DIR at a checkout)" >&2
	echo "       This is a hard failure, not a skip: without the corpus this guard" >&2
	echo "       would compare the committed matrix against nothing and pass." >&2
	echo "wasm corpus-column guard: FAILED" >&2
	exit 1
fi

declare -A CORPUS
corpus_total=0
corpus_families=0
while IFS=$'\t' read -r fam n; do
	[[ -n "$fam" ]] || continue
	CORPUS["$fam"]="$n"
	corpus_total=$((corpus_total + n))
	corpus_families=$((corpus_families + 1))
done <<<"$counts"

# Non-vacuity. `(root)` is always emitted, so a corpus directory that exists but
# holds nothing still yields one family with a zero count — which would let the
# row-set check run over a single empty row. An empty corpus is a broken input,
# not a passing state.
if ((corpus_total == 0)); then
	fail "the corpus at '$conformance_dir' contains no .json fixtures. There is nothing to audit the Corpus column against; a matrix compared to an empty corpus is not verified."
	echo "wasm corpus-column guard: FAILED" >&2
	exit 1
fi

# ── the committed table ─────────────────────────────────────────────────────
body=""
rc=0
body="$(wasm_matrix_committed_body "$wasm_md")" || rc=$?
if ((rc == 2)); then
	fail "WASM.md not found at '$wasm_md'"
	echo "wasm corpus-column guard: FAILED" >&2
	exit 1
fi
if ((rc == 3)); then
	fail "WASM.md is missing the '$WASM_MATRIX_START_MARKER' / '$WASM_MATRIX_END_MARKER' markers the generator writes between, so there is no matrix to audit."
	echo "wasm corpus-column guard: FAILED" >&2
	exit 1
fi

declare -A TABLE TABLE_LINE
table_rows=0
total_line=""
total_cell=""

while IFS= read -r line; do
	[[ -n "${line//[[:space:]]/}" ]] || continue
	[[ "$line" == '| Family |'* ]] && continue
	[[ "$line" == '|---'* ]] && continue
	if [[ "$line" =~ ^\|[[:space:]]*\`([^\`]+)\`[[:space:]]*\|[[:space:]]*([0-9]+)[[:space:]]*\| ]]; then
		fam="${BASH_REMATCH[1]}"
		cell="${BASH_REMATCH[2]}"
		if [[ -n "${TABLE[$fam]+x}" ]]; then
			fail "the WASM.md matrix carries two rows for family '$fam'."
			continue
		fi
		TABLE["$fam"]="$cell"
		TABLE_LINE["$fam"]="$line"
		table_rows=$((table_rows + 1))
	elif [[ "$line" =~ ^\|[[:space:]]*\*\*total\*\*[[:space:]]*\|[[:space:]]*\*\*([0-9]+)\*\*[[:space:]]*\| ]]; then
		total_cell="${BASH_REMATCH[1]}"
		total_line="$line"
	else
		# Never skipped. A row this cannot read is a row it cannot audit, and
		# quietly ignoring it is how the audit would shrink without saying so.
		fail "unparseable row in the WASM.md matrix, so it cannot be audited: $line"
	fi
done <<<"$body"

if ((table_rows == 0)); then
	fail "the WASM.md matrix has no family rows between its markers. Run 'make wasm-matrix' — an empty table is not a matrix with nothing to say."
	echo "wasm corpus-column guard: FAILED" >&2
	exit 1
fi

# ── the two-directional row-set audit + the Corpus column ───────────────────
#
# Equality is asserted against the bytes scripts/check-wasm-tiers.sh's generator
# emits for those cells (wasm_matrix_corpus_cells), not against a second opinion
# about how to spell a row.
for fam in $(printf '%s\n' "${!CORPUS[@]}" | sort); do
	expected="$(wasm_matrix_corpus_cells "$fam" "${CORPUS[$fam]}")"
	if [[ -z "${TABLE[$fam]+x}" ]]; then
		fail "the canonical corpus carries family '$fam' (${CORPUS[$fam]} fixture(s)) and the WASM.md matrix has no row for it. A family that never reaches the table is invisible in the published capability matrix. Run 'make wasm' (needs emsdk) and commit the regenerated table."
		continue
	fi
	if [[ "${TABLE_LINE[$fam]}" != "$expected "* ]]; then
		fail "WASM.md says family '$fam' has ${TABLE[$fam]} canonical fixture(s); the corpus at '$conformance_dir' has ${CORPUS[$fam]}. The committed matrix is stale. Run 'make wasm' (needs emsdk) and commit the regenerated table — the per-tier columns move with this count."
	fi
done

for fam in $(printf '%s\n' "${!TABLE[@]}" | sort); do
	if [[ -z "${CORPUS[$fam]+x}" ]]; then
		fail "the WASM.md matrix has a row for family '$fam', which the canonical corpus at '$conformance_dir' does not contain — the row describes a family that is gone."
	fi
done

if [[ -z "$total_line" ]]; then
	fail "the WASM.md matrix has no **total** row, so the column totals are unaudited."
else
	expected_total="$(wasm_matrix_total_corpus_cells "$corpus_total")"
	if [[ "$total_line" != "$expected_total "* ]]; then
		fail "the WASM.md matrix totals $total_cell canonical fixtures; the corpus has $corpus_total."
	fi
fi

if ((status != 0)); then
	echo "wasm corpus-column guard: FAILED" >&2
	exit 1
fi

echo "wasm corpus-column guard: OK (corpus column + row set only) — $table_rows family rows, $corpus_total canonical fixtures in '$conformance_dir'"
echo "  NOT audited here: the 'wasm core' and 'wasm threaded' columns, the per-tier"
echo "  notes, the WASM_ABSENT_REASONS ledger, and the per-tier fixture floors."
echo "  Those need runtime manifests from an emscripten run and are audited only by"
echo "  scripts/check-wasm-tiers.sh (make wasm / the 'wasm' CI job). This rung is not"
echo "  a full matrix verdict."
