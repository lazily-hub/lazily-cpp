#!/usr/bin/env bash
# Shared definitions for the WASM.md capability matrix (`#lzcppwasmguardlocal`).
#
# Sourced by BOTH matrix guards, and it exists so they cannot disagree:
#
#   scripts/check-wasm-tiers.sh          full-fidelity audit; needs the emsdk
#                                        runtime manifests, runs in the `wasm`
#                                        CI job, and OWNS the generated table
#   scripts/check-wasm-corpus-column.sh  the corpus-derivable subset; needs no
#                                        toolchain at all and runs in `make check`
#
# The split is not arbitrary. The `Corpus` column and the family ROW SET are
# functions of ../lazily-spec/conformance alone — no wasm build participates in
# them — while the per-tier columns are only knowable from a manifest an actual
# emscripten run produced. So the corpus half of the audit can be reached
# locally and the tier half cannot.
#
# Two independent implementations of "count the fixtures" would be the same
# defect one level up: the local rung would go green on a count the CI generator
# computes differently, which is precisely the drift the matrix exists to catch.
# Hence one definition of the count (wasm_corpus_counts) and one definition of
# how a count is RENDERED into a row (wasm_matrix_corpus_cells /
# wasm_matrix_total_corpus_cells), used by the generator and by the comparator.

# Deterministic collation. `sort` orders `(root)` differently under a UTF-8
# locale than under C, so without this the row order — and therefore the
# generated table — depends on the machine.
export LC_ALL=C

WASM_MATRIX_START_MARKER='<!-- wasm-matrix:start -->'
WASM_MATRIX_END_MARKER='<!-- wasm-matrix:end -->'

# The one place the corpus location is resolved. LAZILY_SPEC_CONFORMANCE_DIR is
# the same override every other conformance script in this repo honours.
wasm_corpus_dir() {
	printf '%s\n' "${LAZILY_SPEC_CONFORMANCE_DIR:-$1/../lazily-spec/conformance}"
}

# THE definition of "count the fixtures". Prints `family<TAB>count` for every
# family the canonical corpus carries, `(root)` included, in generated-row order.
#
# Returns 2 — never an empty success — when the corpus directory is missing or
# unreadable. A count function that answers "no families" for an absent corpus
# is how a guard ends up auditing zero rows and reporting OK; the callers decide
# what to do about it, but they cannot mistake absence for emptiness.
wasm_corpus_counts() {
	local dir="$1" d n
	[[ -d "$dir" && -r "$dir" && -x "$dir" ]] || return 2
	while IFS= read -r d; do
		n="$(find "$dir/$d" -name '*.json' | wc -l | tr -d '[:space:]')"
		printf '%s\t%s\n' "$d" "$n"
	done < <(find "$dir" -mindepth 1 -maxdepth 1 -type d -printf '%f\n' | sort)
	n="$(find "$dir" -maxdepth 1 -name '*.json' | wc -l | tr -d '[:space:]')"
	printf '%s\t%s\n' '(root)' "$n"
}

# THE rendering of a matrix row's corpus-derivable prefix: the family cell plus
# the Corpus cell. check-wasm-tiers.sh emits rows through this, and
# check-wasm-corpus-column.sh compares committed rows against it, so a change to
# the table's shape cannot make the two guards read the same row differently.
wasm_matrix_corpus_cells() {
	printf '| `%s` | %d |' "$1" "$2"
}

wasm_matrix_total_corpus_cells() {
	printf '| **total** | **%d** |' "$1"
}

# The committed table body, between the generator's markers. Prints nothing and
# returns non-zero when the file or either marker is absent.
wasm_matrix_committed_body() {
	local md="$1"
	[[ -f "$md" ]] || return 2
	grep -qF "$WASM_MATRIX_START_MARKER" "$md" || return 3
	grep -qF "$WASM_MATRIX_END_MARKER" "$md" || return 3
	awk -v s="$WASM_MATRIX_START_MARKER" -v e="$WASM_MATRIX_END_MARKER" \
		'index($0,s){f=1;next} index($0,e){f=0} f' "$md"
}
