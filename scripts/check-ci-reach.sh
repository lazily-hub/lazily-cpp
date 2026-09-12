#!/usr/bin/env bash
# CI-reachability guard (#lzcheckcireachguard).
#
# Fails the build when `make check` runs a gate that CI never reaches. That is the
# drift this guard exists for: someone adds a target to `check`, it passes locally
# forever, and no CI job ever executes it — which is exactly how #lzinteroppeerci
# happened. The interop peer, the single cross-binding wire-compatibility gate, was
# in every binding's `check` and in no binding's workflow, for months.
#
# It also exists because the obvious hand-audit is WRONG. Grepping the workflows
# for "make check" reported all nine bindings as covered; every one of those hits
# was a COMMENT. Comments are the reason this is a script and not a convention:
# only `run:` bodies count here, and comment lines inside them are stripped before
# anything is matched.
#
# WHAT IT PROVES
#
#   For every target in `check`'s prerequisite closure, at least one CI `run:`
#   step invokes the same program with the same distinguishing flags.
#
#   And, since #pinreachclosure, three things about the closure ITSELF, because
#   the claim above is only worth what the closure is worth. The anchors prove CI
#   runs what `check` runs; these prove `check` still runs what it is supposed to:
#
#     ORACLE          every target in the awk-derived closure has its anchors in
#                     `make -n check`'s anchor list. This is the load-bearing
#                     one — the closure is scanned out of Makefile SOURCE and
#                     cannot see a make conditional, so without asking make the
#                     two pins below are set-equal to a set that describes
#                     nothing. Measured: an `ifeq` decoupling ran the interop
#                     peer zero times with the whole verdict byte-identical.
#     MEMBERSHIP      the closure equals EXPECTED_CLOSURE_TARGETS, by set
#                     equality, reported in both directions by name.
#     CLASSIFICATION  the `no gate` set equals EXPECTED_NO_GATE_TARGETS. That
#                     bucket is the only one this guard excuses from CI reach by
#                     design, which makes it the only one worth moving a gate
#                     into: neuter a recipe to `true` and the target keeps its
#                     name, keeps its membership, and loses its obligation.
#
#   What none of the three prove is the SHAPE of the graph — which target runs
#   before which — or WHICH gate a target's recipe runs. Both are stated where
#   they are enforced, or not, further down.
#
# WHAT IT DOES NOT PROVE
#
#   That CI runs it against the same inputs, in the same environment, or that the
#   command means the same thing there. Reach is a floor, not equivalence. The
#   sibling guards (conformance-coverage, assertion-keys, scenario-coverage) are
#   what prove a run examined anything.
#
# HOW A TARGET IS MATCHED
#
#   Recipes are read through `make -n`, so make variables are already expanded and
#   we compare real command lines rather than source text. `make -p` is
#   deliberately NOT used: it dumps the entire environment to stdout, which would
#   print every secret in the job's env into the CI log.
#
#   Each command is split on the shell's sequencing operators, redirections are
#   dropped, and the remainder is reduced to an ANCHOR: the program basename plus
#   its subcommands and flag NAMES (values dropped), with path arguments reduced to
#   basenames and bare path globs discarded. A target is reached when EVERY one of
#   its anchors is a subsequence of some CI command's token list, or when CI runs
#   `make <target>` directly. Every, not any: a target that runs two gates and is
#   half-covered by CI is a gap, and "any" would report it green.
#
#   Keeping flag names in the anchor is what makes the guard falsifiable rather
#   than decorative: `go test -race` does not match a CI step that only runs
#   `go test -count=1`, so dropping the race job reddens this guard instead of
#   being absorbed by the plain test job.
#
#   An argument that is still a VARIABLE reference at this point — `$MANIFEST` in
#   a CI step, or a `$$VAR` a recipe leaves for the shell — names a value the
#   guard cannot resolve, so it becomes a WILDCARD matching exactly one token on
#   the other side (#lzcireachvaranchor). Make and CI routinely spell the same
#   path differently, one through an expanded `$(VAR)` and the other through the
#   environment, and they are the same command. Dropping the token instead, which
#   is what this used to do, lost the argument as well as its value and reported
#   a step that genuinely ran the gate as unreachable — a false RED that cost one
#   binding a hardcoded second spelling of the path plus a hand-written equality
#   assertion, which is a new drift surface invented to satisfy a guard whose job
#   is detecting drift. Arity still counts: `script.sh $A` does not match a CI
#   step that passes no argument at all.
#
#   Commands whose program is a shell builtin or a plain file/text utility carry no
#   gate, so they contribute no anchor. A target with no non-trivial command at all
#   (a mkdir-only reset step, say) is reported as carrying no gate and is not
#   required to appear in CI. It cannot fail a build, so it cannot hide one.
#
# THE EXCUSE LIST IS THE OTHER HALF OF THE DELIVERABLE
#
#   scripts/ci-reach.conf names the workflows that count and the targets that are
#   deliberately local-only, each with a reason. It is the one place a reader can
#   see what this binding does not enforce in CI, in the same spirit as
#   KNOWN_UNCOVERED. Excuses are checked in THREE directions, and each one closed
#   a way the list could report coverage while doing nothing:
#
#     * an excused target CI turns out to reach fails, so the list cannot rot
#       into a list of things that used to be true;
#     * an excuse with no reason fails, because an excuse without a reason is
#       not an excuse;
#     * an excuse naming a target OUTSIDE the audited closure fails
#       (#pinreachclosure). Excuses are only ever consulted from inside the
#       loop that walks the closure, so one naming anything else was a silent
#       no-op — measured at exit 0 with output byte-identical to a clean run.
#       That is the same reverse check KNOWN_UNCOVERED has always applied to
#       itself, arriving here years late.
set -euo pipefail

MAKE_BIN="${MAKE:-make}"
ROOT_TARGET="${CI_REACH_ROOT_TARGET:-check}"
CONF="${CI_REACH_CONF:-scripts/ci-reach.conf}"

# ------------------------------------------------------------ the closure PIN
#
# Everything above this line audits the closure of $ROOT_TARGET. Nothing above
# it pins WHICH targets are in that closure, and that is a hole the size of the
# guard (#pinreachclosure). Measured on this tree at exit 0: delete
# `test-interop-peer` from `check:`'s prerequisite list and this guard prints
#
#   check-ci-reach: OK — 8 target(s) reached by CI, 0 excused, 1 carrying no gate
#
# and never names the target that left. The interop peer is the single
# cross-binding wire-compatibility gate — the one #lzinteroppeerci was about —
# and dropping its edge turns this guard from the thing that noticed into the
# thing that approves. A gate stops being required and the audit shrinks by one
# with no line of output saying so.
#
# It is worse here than in most bindings. lazily-cpp has no completion marker of
# its own: what stands in for one is the `conformance-coverage: test` graph edge
# plus the record-count rung in check-conformance-coverage.sh. Both live in the
# Makefile's prerequisite graph, so the graph IS this binding's evidence that a
# gate ran — and until this pin existed the graph could be edited without a
# verdict changing.
#
# SET EQUALITY, deliberately, not a count floor and not a ceiling. A floor
# passes a SWAP (drop the interop peer, add a formatting target, count holds). A
# ceiling starts with zero slack and gains some on every legitimate migration
# until the same attack passes again. The property that has to hold is
# fails-when-stale, never passes-when-stale — the same reasoning that replaced
# MAX_LEDGERED_BLOCKS with EXPECTED_LEDGERED_BLOCKS in this family, and the
# reason the name here carries the `EXPECTED_` prefix rather than MIN_ or KNOWN_.
#
# WHAT THIS PIN DOES NOT SEE, stated because it is easy to assume otherwise.
# It pins closure MEMBERSHIP, which is a set, not the graph's SHAPE. Measured:
# dropping the `test` edge from `conformance-coverage:` leaves this guard's whole
# output byte-identical (9 reached / 0 excused / 1 no gate, exit 0) both before
# and after this pin, because `test` is still in the closure via `check:`'s own
# prerequisite list. Which targets run is pinned here; which target runs BEFORE
# which is not, and the ordering hazard that edge exists to close
# (#lzstalemanifest, `make -j` free to start the coverage guard ahead of the
# suite) is held by the coverage guard's own run-id and exact magnitudes, not by
# anything in this file.
EXPECTED_ROOT_TARGET="${CI_REACH_EXPECTED_ROOT_TARGET:-check}"

# Every target in `make check`'s prerequisite closure, sorted (LC_ALL=C), root
# included. The root is a member of its own closure and belongs here: without it
# a rename could empty the set from the other end. $EXPECTED_ROOT_TARGET pins the
# name this guard walks FROM; this list pins where walking it arrives.
#
# 10 targets, which is exactly today's 9 reached + 0 excused + 1 carrying no
# gate. That identity is re-proved on every run by the accounting rung at the
# bottom, so a future `continue` that skips a target cannot quietly drop it out
# of the audit the way an unreadable recipe once did (#lzgrepcpipefail).
#
# Changing `check:` requires changing this list in the same commit. That is the
# point, and the diagnostics below are written to keep the two reasons apart: a
# target MISSING from the closure is a gate that left, and a target EXTRA in the
# closure is a gate that arrived unpinned. Only one of those is ever fixed by
# editing this list.
EXPECTED_CLOSURE_TARGETS=(
  assertion-ordering-check
  build
  check
  ci-reach
  configure
  conformance-coverage
  fmt
  test
  test-interop-peer
  wasm-corpus-column
)

# ── the STEP PIN: which CI STEP runs each gate (#reversereachdirection) ────
#
# One entry per ANCHOR-REACHED closure member, `target|job|step name`. Reach for
# that member is then asked INSIDE this step instead of against a flat set of
# every `run:` body in the workflow — see the header on ci_commands_scoped for
# the two attacks the flat question let through, both measured live here.
#
# Only anchor-reached members appear. `fmt` and `assertion-ordering-check` are
# reached by CI invoking them THROUGH MAKE (`make fmt`, `make
# assertion-ordering-check`), so they have no independent CI-side spelling and
# pinning a step for one of them would assert nothing: the step would be
# believed because it says `make <target>`, which is the same fact
# make_invokes() already reads. Refused rather than mapped, by the rung below,
# and the refusal is symmetric — a pin here for a make-invoked target is a hard
# failure, so this cannot rot into a mapping that looks like coverage.
#
# Two, measured from `run:` bodies with comments AND quoted strings excluded, not
# from a grep over the workflow text. A grep finds 19 occurrences of `make` in
# ci.yml; only FIVE are invocations (`make fmt`, `make assertion-ordering-check`
# twice, `make wasm-core`, `make wasm-threaded`) and of those only two name a
# closure member. Twelve of the other fourteen are comments and two are quoted
# strings — `refuse "make assertion-ordering-check"` is a label passed to a shell
# function, and `echo "::error::a make check gate is unreachable from CI"` is a
# message. This is the same trap the workflow's own comment on the guard step
# records, and it is why the split above is derived from ci_commands_scoped
# rather than counted by eye.
#
# The job id is part of the key because step names are NOT unique across jobs:
# this workflow has 20 `run:` steps and 19 distinct names — the canonical
# lazily-spec fixture fetch is spelled identically in the `test` and `wasm`
# jobs. The pair is also required to resolve to EXACTLY ONE step, so a future
# duplicate inside one job is refused instead of silently widening a pinned
# scope back out.
#
# CHURN is step-name-rate, not recipe-rate. A recipe gaining a flag moves the
# Makefile and the CI step together and this mapping does not move; it moves
# when a step is renamed, split or deleted, which is exactly when a reader
# should be asked whether the gate still runs.
#
# What this does NOT close, said plainly rather than implied: a recipe weakened
# INSIDE its own pinned step — `ctest` losing `--output-on-failure`, the interop
# peer losing `--self-check` — stays green, because anchors match as
# subsequences and extra CI-side tokens are allowed by design. Closing that
# needs a per-recipe-content pin, which is a second spelling of every recipe in
# this file and is declined for the reason the anchors() header gives.
EXPECTED_GATE_STEPS=(
  "build|test|Build"
  "ci-reach|test|CI-reachability guard (#lzcheckcireachguard)"
  "configure|test|Configure"
  "conformance-coverage|test|Assert canonical fixtures and scenarios were replayed (#lzguardsnotinci)"
  "test|test|Test"
  "test-interop-peer|test|Interop peer self-check (#lzinteroppeerci)"
  "wasm-corpus-column|test|WASM.md corpus column + row set (#lzcppwasmguardlocal)"
)

if [ "$ROOT_TARGET" != "$EXPECTED_ROOT_TARGET" ]; then
	echo "check-ci-reach: root target is '$ROOT_TARGET' but EXPECTED_ROOT_TARGET pins '$EXPECTED_ROOT_TARGET'" >&2
	echo "  The closure below, and EXPECTED_CLOSURE_TARGETS with it, describe" >&2
	echo "  '$EXPECTED_ROOT_TARGET'. Auditing a different root would compare one graph's" >&2
	echo "  membership against another's pin — every verdict would be about the wrong" >&2
	echo "  subject. Point the guard back at '$EXPECTED_ROOT_TARGET', or repin both names" >&2
	echo "  together and deliberately." >&2
	exit 1
fi

if [ ! -f Makefile ]; then
	echo "check-ci-reach: no Makefile in $(pwd)" >&2
	exit 1
fi

# ---------------------------------------------------------------- configuration

workflows=()
workflow_count=0
excused_targets=()
excused_reasons=()
excuse_count=0

if [ -f "$CONF" ]; then
	while IFS= read -r line || [ -n "$line" ]; do
		line="${line%%$'\r'}"
		case "$line" in
		'#'* | '') continue ;;
		esac
		key="${line%%:*}"
		val="${line#*:}"
		val="$(printf '%s' "$val" | sed -e 's/^[[:space:]]*//' -e 's/[[:space:]]*$//')"
		case "$key" in
		workflow)
			workflows+=("$val")
			workflow_count=$((workflow_count + 1))
			;;
		excuse)
			tgt="${val%%[[:space:]]*}"
			reason="${val#"$tgt"}"
			reason="$(printf '%s' "$reason" | sed -e 's/^[[:space:]]*//')"
			if [ -z "$reason" ]; then
				echo "check-ci-reach: excuse for '$tgt' has no reason — an excuse without a reason is not an excuse" >&2
				exit 1
			fi
			excused_targets+=("$tgt")
			excused_reasons+=("$reason")
			excuse_count=$((excuse_count + 1))
			;;
		*)
			echo "check-ci-reach: unknown key '$key' in $CONF" >&2
			exit 1
			;;
		esac
	done <"$CONF"
fi

if [ "$workflow_count" -eq 0 ]; then
	workflows=(".github/workflows/ci.yml")
	workflow_count=1
fi

for wf in "${workflows[@]}"; do
	if [ ! -f "$wf" ]; then
		echo "check-ci-reach: workflow '$wf' listed in $CONF does not exist" >&2
		exit 1
	fi
done

# ------------------------------------------------------- make target extraction

# A Makefile may set .RECIPEPREFIX to something other than tab (lazily-rs uses
# `>`), which puts recipe lines at column 0 where a rule line lives. Without this
# a recipe such as `>cargo test --features a:b` reads as a rule named `>cargo`.
RECIPE_PREFIX="$(awk -F= '/^[[:space:]]*\.RECIPEPREFIX[[:space:]]*[:+]?=/ {
	v = $2; gsub(/^[[:space:]]+|[[:space:]]+$/, "", v); if (v != "") print substr(v, 1, 1); exit
}' Makefile)"

# Prerequisites of a target, straight from the Makefile source, with `\`
# continuations joined and trailing comments removed. Order-only prerequisites are
# dropped: they constrain ordering, not what runs.
prereqs_of() {
	awk -v target="$1" -v rp="$RECIPE_PREFIX" '
		BEGIN { pat = "^" target ":([^=]|$)"; if (rp == "") rp = "\t" }
		{
			line = $0
			# Only the ACTUAL recipe prefix marks a recipe line. Treating any
			# leading whitespace as one loses a rule that is merely indented,
			# which under a non-tab .RECIPEPREFIX is perfectly legal make and
			# collapses the whole closure to a single target. A continuation is
			# exempt: under the default tab prefix a wrapped prerequisite list is
			# normally tab-indented.
			if (!cont && substr(line, 1, 1) == rp) next
			sub(/^[[:space:]]+/, "", line)
			if (cont) {
				buf = buf " " line
				if (line ~ /\\[[:space:]]*$/) next
				cont = 0
				emit(buf)
				exit
			}
			if (line !~ pat) next
			buf = line
			if (line ~ /\\[[:space:]]*$/) { cont = 1; next }
			emit(buf)
			exit
		}
		function emit(s,   rest, n, i, parts) {
			gsub(/\\/, " ", s)
			sub(/#.*$/, "", s)
			rest = substr(s, index(s, ":") + 1)
			sub(/\|.*$/, "", rest)
			n = split(rest, parts, /[[:space:]]+/)
			for (i = 1; i <= n; i++) if (parts[i] != "") print parts[i]
		}
	' Makefile
}

# Is this name an explicit rule in the Makefile?
is_makefile_target() {
	awk -v target="$1" -v rp="$RECIPE_PREFIX" '
		BEGIN { pat = "^" target ":([^=]|$)"; if (rp == "") rp = "\t"; found = 0 }
		substr($0, 1, 1) == rp { next }
		{ line = $0; sub(/^[[:space:]]+/, "", line) }
		line ~ pat { found = 1; exit }
		END { exit found ? 0 : 1 }
	' Makefile
}

# Breadth-first closure of ROOT_TARGET's prerequisites, parents before children.
closure=""
queue="$ROOT_TARGET"
seen=" "
while [ -n "$queue" ]; do
	current="${queue%%$'\n'*}"
	if [ "$current" = "$queue" ]; then queue=""; else queue="${queue#*$'\n'}"; fi
	[ -n "$current" ] || continue
	case "$seen" in
	*" $current "*) continue ;;
	esac
	seen="$seen$current "
	closure="$closure$current"$'\n'
	while IFS= read -r dep; do
		[ -n "$dep" ] || continue
		if is_makefile_target "$dep"; then
			queue="$queue$dep"$'\n'
		fi
	done < <(prereqs_of "$current")
done

# ── the closure pin, enforced ───────────────────────────────────────────────
#
# Compared as SETS, and reported in BOTH directions by name, because the two
# directions are two different mistakes with two different fixes:
#
#   pinned but NOT in the closure  — a gate left `$ROOT_TARGET`'s prerequisites
#                                    (deleted, renamed, or moved behind a
#                                    condition). The audit just shrank.
#   in the closure but NOT pinned  — a gate arrived without being pinned. The
#                                    audit grew, which is fine, but silently,
#                                    which is how it shrinks again later.
#
# A rename fires both arms at once, which is exactly the report a reader wants.
#
# This is a HARD exit rather than a contribution to $status below, and that is
# the same call the root `make -n` probe makes for the same reason: a closure
# that is not the pinned set means every verdict underneath is about a different
# graph. Printing `reached` lines for a shrunken audit and an error beside them
# would invite reading the OK half.
#
# `in_closure` reads $seen, which the walk above maintains as the space-delimited
# membership of the closure — the same string the walk itself de-duplicates on,
# so the pin and the audit loop can never disagree about who is a member.
in_closure() {
	case "$seen" in
	*" $1 "*) return 0 ;;
	esac
	return 1
}

# A root that is not an explicit rule at all. The walk starts from $ROOT_TARGET
# unconditionally, so a renamed `check:` leaves a closure of exactly one member
# and the missing-arm below would print nine "a gate left check" lines — true in
# a sense, and the wrong subject. Name the real cause. `make -n` would also fail
# here and the root probe further down says so, but that probe runs after the
# workflows are scraped and this costs nothing: the closure is read from Makefile
# SOURCE, so this verdict is available before make is invoked at all.
if ! is_makefile_target "$ROOT_TARGET"; then
	echo "check-ci-reach: '$ROOT_TARGET' is not an explicit rule in Makefile" >&2
	echo "  The closure this guard audits starts there, so with no such rule it contains" >&2
	echo "  exactly one member and every pinned gate reads as dropped. The root was" >&2
	echo "  renamed or removed: restore it, or repin EXPECTED_ROOT_TARGET and" >&2
	echo "  EXPECTED_CLOSURE_TARGETS together." >&2
	exit 1
fi

pin_count="${#EXPECTED_CLOSURE_TARGETS[@]}"

# An emptied pin would be caught by the extra-direction arm below anyway — the
# root is always a member, so the closure is never empty and every member would
# report as unpinned. Refused explicitly all the same, because that arm's wording
# ("added without being pinned") would be a false story about what happened.
if [ "$pin_count" -eq 0 ]; then
	echo "check-ci-reach: EXPECTED_CLOSURE_TARGETS is empty — a set-equality pin against the empty set pins nothing" >&2
	exit 1
fi

pin_missing=""
pin_missing_count=0
for expected_target in "${EXPECTED_CLOSURE_TARGETS[@]}"; do
	if ! in_closure "$expected_target"; then
		pin_missing="$pin_missing$expected_target"$'\n'
		pin_missing_count=$((pin_missing_count + 1))
	fi
done

pin_extra=""
pin_extra_count=0
while IFS= read -r target; do
	[ -n "$target" ] || continue
	found=0
	for expected_target in "${EXPECTED_CLOSURE_TARGETS[@]}"; do
		if [ "$expected_target" = "$target" ]; then
			found=1
			break
		fi
	done
	if [ "$found" -eq 0 ]; then
		pin_extra="$pin_extra$target"$'\n'
		pin_extra_count=$((pin_extra_count + 1))
	fi
done <<<"$closure"

if [ "$((pin_missing_count + pin_extra_count))" -ne 0 ]; then
	echo "check-ci-reach: '$ROOT_TARGET' closure does not equal EXPECTED_CLOSURE_TARGETS" >&2
	if [ "$pin_missing_count" -gt 0 ]; then
		echo >&2
		echo "  $pin_missing_count pinned target(s) are NOT in the closure — a gate left '$ROOT_TARGET':" >&2
		while IFS= read -r t; do
			[ -n "$t" ] || continue
			echo "    - $t" >&2
		done <<<"$pin_missing"
		echo "  Either restore its prerequisite edge in the Makefile — the likely case, and" >&2
		echo "  the reason this pin exists — or, if you meant to retire the gate, delete it" >&2
		echo "  from EXPECTED_CLOSURE_TARGETS in the same commit and say why." >&2
	fi
	if [ "$pin_extra_count" -gt 0 ]; then
		echo >&2
		echo "  $pin_extra_count closure target(s) are NOT pinned — a gate arrived unpinned:" >&2
		while IFS= read -r t; do
			[ -n "$t" ] || continue
			echo "    - $t" >&2
		done <<<"$pin_extra"
		echo "  Add it to EXPECTED_CLOSURE_TARGETS (sorted) so a later commit cannot drop it" >&2
		echo "  again without this guard noticing." >&2
	fi
	if [ "$pin_missing_count" -gt 0 ] && [ "$pin_extra_count" -gt 0 ]; then
		echo >&2
		echo "  Both arms fired, which is what a RENAME looks like: repin the new name and" >&2
		echo "  drop the old one, and check the workflow still runs it under its new name." >&2
	fi
	exit 1
fi

# An excuse for a target outside the closure — the mirror of the hole above, and
# the direction dart found. `is_excused` is only ever consulted from inside the
# audit loop, which walks the closure, so an excuse naming anything else was a
# silent no-op: it reported `0 excused` and complained about nothing, while the
# conformance guard's KNOWN_UNCOVERED has always checked its own reverse
# direction ("lists 'X', which is not in the canonical corpus"). Measured here at
# exit 0 with output byte-identical to a clean run: `excuse: bench ...` plus
# `excuse: no-such-target-at-all ...` changed nothing.
#
# It matters because an excuse is how a gate is legitimately allowed to be
# unreached. One that names a target this guard never examines is either a stale
# excuse for a gate that already left `$ROOT_TARGET` — in which case the pin
# above is the honest report and this excuse would have hidden the story — or a
# typo, which means the gate it MEANT to excuse is still being audited under its
# real name and the excuse is doing nothing anyone can see.
excuse_outside=0
for i in "${!excused_targets[@]}"; do
	t="${excused_targets[$i]}"
	if ! in_closure "$t"; then
		if [ "$excuse_outside" -eq 0 ]; then
			echo "check-ci-reach: $CONF excuses target(s) outside '$ROOT_TARGET''s closure:" >&2
		fi
		echo "  - '$t' — ${excused_reasons[$i]}" >&2
		excuse_outside=$((excuse_outside + 1))
	fi
done
if [ "$excuse_outside" -ne 0 ]; then
	echo "  An excuse is a claim about a gate 'make $ROOT_TARGET' RUNS but CI does not." >&2
	echo "  This guard only ever consults excuses for closure members, so an excuse for" >&2
	echo "  anything else excuses nothing and reports nothing — it reads as coverage" >&2
	echo "  while doing no work. Fix the target name, or drop the excuse." >&2
	exit 1
fi

printf 'pinned   closure of %s = EXPECTED_CLOSURE_TARGETS (%s target(s))\n' "$ROOT_TARGET" "$pin_count"

# `make -n` for a target emits its prerequisites' commands first, then its own.
# Asking make for the prerequisite list alone yields exactly that prefix — make
# applies the same de-duplication to both invocations — so removing it leaves the
# target's own recipe. Diagnostics make writes about targets it has nothing to do
# for are not commands and are dropped.
# A recipe line broken across physical lines with `\` reaches the shell as ONE
# command, and make -n prints it the way the Makefile spells it. Joining here is
# what keeps `VAR=x \` + `go test ./...` from being read as two commands, the
# second of which is where the whole gate lives.
join_continuations() {
	awk '
		{
			line = $0
			if (line ~ /\\[[:space:]]*$/) {
				sub(/\\[[:space:]]*$/, "", line)
				buf = buf line " "
				next
			}
			print buf line
			buf = ""
		}
		END { if (buf != "") print buf }
	'
}

# The `|| true` on the pipeline below is NOT what keeps a recipe that prints no
# command from killing this script, and saying so would be the wrong claim.
# Measured: removing it changes nothing — neither on a clean tree nor under the
# attacks described below — because own_commands() is invoked as
# `$(own_commands ... | anchors | sort -u || true)`, so the function already runs
# in a command substitution's subshell behind a `|| true` of its own. Bash also
# suppresses `set -e` for the whole body of a function called as a non-last
# member of an AND-OR list. This `|| true` is insurance against a future
# call-site change, and zero commands is a legitimate measurement here — the
# `no gate` bucket exists for it. Those are different claims; only the second
# one is load-bearing today.
#
# What `|| true` must NOT absorb is `make -n` itself failing (#lzgrepcpipefail).
# That is a STATUS, not a measurement, and it arrived here wearing the same
# clothes: an empty command list, which files the target under `no gate` — the
# ONE bucket the vacuity rung at the bottom does not count. So a target whose
# recipe could not be listed had its CI-reach obligation skipped rather than
# failed and the guard still printed OK. Measured three ways, each at exit 0
# before the probes below existed: a prerequisite with no rule
# (`test-interop-peer: build no-such-prerequisite`) gave `OK — 8 reached, 2 no
# gate`; a goal-conditional prerequisite on `conformance-coverage` gave the same
# while `make -n check` itself still exited 0; and an `excuse:` line for it
# changed nothing, because the emptiness test already preceded the excuse
# consultation.
#
# That status is NOT read here. It is read by two explicit `make -n` probes —
# one for the root, one per closure target — that run in the MAIN shell BEFORE
# any recipe is read, so an unreadable recipe is classified before it can look
# like an empty one. This function could not do it: own_commands() is called
# inside a command substitution, so a `return 1` or an `exit` from here reaches
# only a subshell and the caller's own `|| true` swallows it.
#
# The multi-goal `dry_run "${deps[@]}"` call below is deliberately NOT probed.
# `make -n` with several goals sets MAKECMDGOALS to the whole list, so a HEALTHY
# goal-conditional prerequisite can behave differently there than in any real
# invocation — probing it manufactures false reds. And there is no safety given
# up: that call only measures how many output lines belong to the
# prerequisites, so when it fails the target is credited with its
# prerequisites' anchors as well, which OVER-reports and therefore fails closed.
dry_run() {
	"$MAKE_BIN" -n "$@" 2>/dev/null | grep -v -e '^make\[' -e '^make:' | join_continuations || true
}

own_commands() {
	local target="$1"
	local deps=()
	local dep_count=0
	while IFS= read -r dep; do
		[ -n "$dep" ] || continue
		if is_makefile_target "$dep"; then
			deps+=("$dep")
			dep_count=$((dep_count + 1))
		fi
	done < <(prereqs_of "$target")

	if [ "$dep_count" -eq 0 ]; then
		dry_run "$target"
		return
	fi
	local prefix
	prefix="$(dry_run "${deps[@]}" | wc -l)"
	dry_run "$target" | tail -n +"$((prefix + 1))"
}

# ------------------------------------------------------------- workflow scraping

# Command lines from every `run:` step, each prefixed with the JOB id and the
# STEP NAME that runs it, US-separated. Comment lines inside a run body are
# stripped here — the whole reason this guard is a script.
#
# The attribution is what makes the step pin below possible
# (#reversereachdirection). Before it, every `run:` body in every job collapsed
# into ONE flat anchor set and reach meant "some CI command anywhere contains
# this target's anchors". That question is too weak in two separate ways, and
# both were measured live on this tree:
#
#   * A member's recipe repointed at a real workflow step that NO member runs
#     satisfies it. `test-interop-peer:` running check-wasm-tiers.sh — a real
#     step in the wasm job — gave output byte-identical to healthy at exit 0
#     with `make -n check` running the interop peer ZERO times.
#   * A NARROW step's command can be a SUPERSET of a BROAD member's anchor, so
#     deleting the broad member's own step leaves the guard green. THREE live
#     instances here, each byte-identical to healthy at exit 0: the `Test` step
#     (the whole 64/64 ctest suite) survived deletion because the fail-closed
#     proof step runs `ctest --test-dir build -R ReactiveGraphConformance
#     --output-on-failure --no-tests=error`, which contains `ctest --test-dir
#     build --output-on-failure` as an in-order subsequence; the `WASM.md corpus
#     column` step survived because the same proof step also runs
#     check-wasm-corpus-column.sh; and the `CI-reachability guard` step — this
#     guard's own step — survived as well.
#
# Scoping reach to the pinned step closes both. It is STRICTER than the flat
# check, so it was measured before being committed: all seven anchor-reached
# members here have a SINGLE own anchor, no member's anchors spread across two
# steps, and nothing legitimate reddened.
#
# An UNNAMED `run:` step cannot be pinned, so it is attributed to the sentinel
# `(unnamed)` rather than silently inheriting the previous step's name — a
# carried-over name would map a gate to a step that does not run it, which is
# the exact failure this pin exists to stop. cpp has none today; the sentinel is
# what keeps that a measurement instead of an assumption.
#
# One `\001runstep` line is emitted per `run:` step, ahead of its commands, so
# the step INVENTORY and the step's COMMANDS come out of a single parse. The pin
# needs the inventory to prove a pinned step exists and is unique, and a second
# scraper for it would be a second thing to drift.
ci_commands_scoped() {
	awk '
		function flush() { if (buf != "") { print job US step US buf; buf = "" } }
		function stepname() { return (name == "" ? "(unnamed)" : name) }
		BEGIN { US = "\037"; job = "(no job)"; name = "" }
		{
			line = $0
			indent = match(line, /[^ ]/) - 1
			if (indent < 0) indent = 9999

			if (inblock) {
				if (line ~ /^[[:space:]]*$/) next
				if (indent <= block_indent) { flush(); inblock = 0 }
				else {
					sub(/^[[:space:]]+/, "", line)
					if (substr(line, 1, 1) == "#") next
					if (line ~ /\\[[:space:]]*$/) {
						sub(/\\[[:space:]]*$/, "", line)
						buf = buf " " line
						next
					}
					if (buf != "") { print job US step US buf " " line; buf = "" }
					else print job US step US line
					next
				}
			}

			# ── attribution, not matching ───────────────────────────────
			# Job ids are indent-2 keys, but only INSIDE `jobs:` — `on:`s
			# `push:` is an indent-2 key too, and reading it as a job id
			# would name a scope no pin could ever match.
			if (line ~ /^jobs:[[:space:]]*$/) { injobs = 1; next }
			if (line ~ /^[^[:space:]#]/) { injobs = 0 }
			if (injobs && line ~ /^  [A-Za-z_][A-Za-z0-9_.-]*:[[:space:]]*$/) {
				j = line; sub(/^[[:space:]]+/, "", j); sub(/:[[:space:]]*$/, "", j)
				job = j
				name = ""
			}
			# A new list item starts a new step, so the name resets with it.
			if (line ~ /^[[:space:]]*-[[:space:]]/) name = ""
			if (line ~ /^[[:space:]]*(-[[:space:]]+)?name:[[:space:]]*[^[:space:]]/) {
				s = line; sub(/^[[:space:]]*(-[[:space:]]+)?name:[[:space:]]*/, "", s)
				sub(/[[:space:]]+$/, "", s)
				name = s
			}
			# ────────────────────────────────────────────────────────────

			if (line ~ /^[[:space:]]*(-[[:space:]]+)?run:[[:space:]]*[|>][-+]?[[:space:]]*$/) {
				inblock = 1
				block_indent = indent
				buf = ""
				step = stepname()
				print job US step US "\001runstep"
				next
			}
			if (line ~ /^[[:space:]]*(-[[:space:]]+)?run:[[:space:]]*[^|>[:space:]]/) {
				sub(/^[[:space:]]*(-[[:space:]]+)?run:[[:space:]]*/, "", line)
				step = stepname()
				print job US step US "\001runstep"
				print job US step US line
			}
		}
		END { flush() }
	' "$@"
}

# ------------------------------------------------------------------- normalizing

# Reduce command text to anchors, one per line, each a space-separated token list.
anchors() {
	awk '
		BEGIN {
			# Sentinel for an unresolvable variable reference. Deliberately not a
			# string any real argument can be.
			ANY = "\001any"
			split(": true false echo printf cd pushd popd mkdir rmdir rm cp mv ln touch " \
			      "export unset set local read eval exec trap wait sleep exit return " \
			      "if then else elif fi for while until do done case esac function " \
			      "test [ [[ pwd ls cat head tail sed awk grep egrep fgrep sort uniq " \
			      "wc tr cut paste tee xargs env dirname basename date git", t, / /)
			for (i in t) if (t[i] != "") trivial[t[i]] = 1
		}
		{
			n = split(split_unquoted($0), cmds, /\n/)
			for (i = 1; i <= n; i++) emit(cmds[i])
		}
		# Split on the shell'"'"'s sequencing operators, but ONLY outside quotes. Doing
		# this before quotes are stripped is what stops a `;` inside a message —
		# `echo "missing $(DIR); clone the sibling"` — from being read as a second
		# command and inventing an anchor for a gate that does not exist. That is a
		# false RED, so it costs a real target its verdict.
		function split_unquoted(s,   i, c, nxt, len, inq, q, out) {
			out = ""; inq = 0; q = ""; len = length(s)
			for (i = 1; i <= len; i++) {
				c = substr(s, i, 1)
				if (inq) {
					if (c == q) { inq = 0; q = "" }
					out = out c
					continue
				}
				if (c == "\"" || c == "'"'"'" || c == "`") { inq = 1; q = c; out = out c; continue }
				nxt = substr(s, i + 1, 1)
				if (c == ";") { out = out "\n"; continue }
				if ((c == "&" && nxt == "&") || (c == "|" && nxt == "|")) { out = out "\n"; i++; continue }
				if (c == "|") { out = out "\n"; continue }
				out = out c
			}
			return out
		}
		function emit(cmd,   m, j, tok, out, prog, started, parts) {
			gsub(/[`"'"'"']/, " ", cmd)
			gsub(/\$\(/, " ", cmd)
			gsub(/\$\{/, " ", cmd)
			gsub(/[(){}]/, " ", cmd)
			m = split(cmd, parts, /[[:space:]]+/)
			prog = ""
			out = ""
			started = 0
			for (j = 1; j <= m; j++) {
				tok = parts[j]
				if (tok == "" || tok == "\\") continue
				if (tok ~ /^[0-9]*>>?$/ || tok == "<" || tok ~ /^[0-9]+>&[0-9]+$/) break
				if (!started) {
					if (tok ~ /^[A-Za-z_][A-Za-z0-9_]*=/) continue
					started = 1
					prog = tok
					sub(/.*\//, "", prog)
					if (prog == "" || (prog in trivial)) return
					out = prog
					continue
				}
				if (tok ~ /^-/) {
					sub(/=.*$/, "", tok)
					out = out " " tok
					continue
				}
				if (tok ~ /^\.{1,3}$/ || tok ~ /^\.{1,2}\/\.{0,3}$/) continue
				if (tok ~ /\//) {
					sub(/\/+$/, "", tok)
					sub(/.*\//, "", tok)
					if (tok == "" || tok ~ /^\.{1,3}$/) continue
				}
					# A token that is still a shell/make VARIABLE reference names a
					# value this guard cannot resolve — a CI step spelling a path as
					# "$LAZILY_CONFORMANCE_MANIFEST" and a Makefile recipe spelling the
					# same path through an expanded $(VAR) are the same command. Dropping
					# it (what this used to do) loses the ARGUMENT as well as its value,
					# so `script.sh <path>` no longer matched a CI step that really ran
					# `script.sh "$PATH"` and the target was reported unreachable. That is
					# a false RED, and it cost lazily-cpp a hardcoded second spelling of
					# the path plus a hand-written equality assertion to keep the two in
					# sync — a new drift surface invented to satisfy a guard that exists
					# to detect drift.
					#
					# Emit a WILDCARD instead: one token that matches one token, so arity
					# is preserved. `script.sh $A` still fails against a CI step that
					# passes no argument at all. This is the same looseness the normalizer
					# already applies to paths, which it reduces to basenames — reach is a
					# floor, not equivalence, exactly as the header says.
					if (substr(tok, 1, 1) == "$") { out = out " " ANY; continue }
				out = out " " tok
			}
			if (started && out != "") print out
		}
	'
}

# --------------------------------------------------------------------- matching

ci_raw="$(mktemp)"
ci_anchor="$(mktemp)"
# The step-scoped scrape (#reversereachdirection): one `job\037step\037anchor`
# line per anchor, and one `job\037step` line per `run:` step for the pin's
# existence/uniqueness rung. $ci_anchor is DERIVED from $ci_scoped by dropping
# the scope, so the flat set make_invokes() reads and the scoped set the reach
# check reads cannot disagree — there is one scrape, not two.
ci_scoped="$(mktemp)"
ci_steps="$(mktemp)"
# The `make -n` probes' scratch: one file collecting every failure for the final
# report, one for the stderr of the probe in flight.
probe_failures="$(mktemp)"
probe_stderr="$(mktemp)"
# The oracle's scratch: the ROOT's real anchor list, a second copy of it for the
# reproducibility check, the target in flight, and the accumulated findings.
oracle_root="$(mktemp)"
oracle_root_again="$(mktemp)"
oracle_target="$(mktemp)"
oracle_target_again="$(mktemp)"
oracle_failures="$(mktemp)"
# One line per audited target: its OWN-recipe anchor set, for the collision rung.
anchor_sets="$(mktemp)"
trap 'rm -f "$ci_raw" "$ci_anchor" "$ci_scoped" "$ci_steps" "$probe_failures" "$probe_stderr" "$oracle_root" "$oracle_root_again" "$oracle_target" "$oracle_target_again" "$oracle_failures" "$anchor_sets"' EXIT

# ── probe 1 of 2: the ROOT ─────────────────────────────────────────────────
#
# A hard exit, not a counted verdict. With the root's recipe graph unreadable
# every target's command list comes back empty, so every one of them files as
# `no gate` and the guard reports OK having audited nothing — measured: with
# MAKE pointed at a missing binary all ten closure targets read `no gate` and
# the vacuity rung then blamed the Makefile's structure, the wrong subject and
# the wrong fix, while `2>/dev/null` inside dry_run discarded the one line that
# named the real cause. An unreadable root does not mean one target dropped, it
# means no verdict below would mean anything, so it is reported as such and
# make's own stderr is printed verbatim.
root_probe_rc=0
"$MAKE_BIN" -n "$ROOT_TARGET" >/dev/null 2>"$probe_stderr" || root_probe_rc=$?
if [ "$root_probe_rc" -ne 0 ]; then
	echo "check-ci-reach: \`$MAKE_BIN -n $ROOT_TARGET\` exited $root_probe_rc — make cannot read the recipe graph this guard audits:" >&2
	sed -e 's/^/      /' "$probe_stderr" >&2
	echo "  This is a hard failure, not a per-target finding. With the root unreadable every" >&2
	echo "  target's command list comes back empty, every one of them files as 'no gate', and" >&2
	echo "  a guard that audited nothing reports OK." >&2
	exit 1
fi
# ── the make-derived ORACLE (#pinreachclosure) ────────────────────────────
#
# The closure above is awk-scanned out of Makefile SOURCE: prereqs_of() finds the
# first line matching `^<target>:` and reads its prerequisites. It never asks
# make, so it cannot see a make CONDITIONAL — and that makes the membership pin
# above, on its own, worthless. lazily-js measured the shape; it reproduces here
# verbatim:
#
#     ifeq ($(SKIP_SLOW),)
#     check: fmt build test test-interop-peer ...   # the ONLY ^check: line read
#     else
#     check: fmt build test ...                     # what make actually parses
#     endif
#
# Measured on this tree: with SKIP_SLOW=1, `make -n check` runs the interop peer
# ZERO times, and this guard's entire output — the `pinned closure of check =
# EXPECTED_CLOSURE_TARGETS (10 target(s))` line included — is byte-identical
# (cmp) to a healthy run, at exit 0. The awk closure is the same in both
# branches, so a set-equality pin over it is the same in both branches and
# passes the compromised state BY CONSTRUCTION. `ifeq (0,1)` does it with no
# variable at all. The pin is not defeated here, it is bypassed: it compares one
# set against another set that has stopped describing what runs.
#
# So the load-bearing check is this one: for every awk-discovered closure target,
# its own anchors must appear in `make -n $ROOT_TARGET`'s anchor list. That
# question goes to MAKE, which parses the conditionals. `make -n` only — never
# `make -p`, which builds the default goal and dumps the whole environment,
# every secret in the job with it.
#
# ANCHORS, NOT RAW COMMAND LINES. js's probe compares `make -n` output verbatim,
# and measured on a PRISTINE Makefile that gives three mismatches — on `test`,
# `conformance-coverage` and `check` — every one of them this line:
#
#   printf '# lazily-run-id %s\n' '2561100-1789233749221496031' > .../conformance-fixtures-loaded.txt
#
# LAZILY_CONFORMANCE_RUN_ID is `?=` then `:=` over `$(shell ...)`, so it is
# minted ONCE PER MAKE INVOCATION (#lzstalemanifest, deliberately). Two separate
# `make -n` runs spell that recipe differently and a verbatim comparison reads a
# healthy tree as decoupled — a permanent false RED on the three targets that
# carry this binding's suite. lazily-gd hit the same wall.
#
# So the comparison runs through anchors(), the same normalizer the CI side
# already uses. That drops the run id for a structural reason rather than a
# convenient one: `printf` is on the trivial-program list at the top of this
# file, so the whole stamping command carries no gate and contributes no anchor
# at all. Measured: with NO environment pinning of any kind, the anchor list of
# all ten closure targets is identical across two consecutive probes.
#
# The alternative considered and rejected was to keep verbatim lines and pin
# LAZILY_CONFORMANCE_RUN_ID in the probe environment. It works — measured — and
# it is stricter, but it teaches the guard one Makefile's variable names and
# hands the next volatile recipe a red that is fixed by adding another env pin.
# Anchoring is the rule the rest of this script already lives by.
#
# It is a real loosening and worth naming: anchors drop flag VALUES, reduce
# paths to basenames, and erase trivial commands entirely, so the oracle asks
# whether the root runs a command SHAPED like the target's, not the identical
# one. Reach is a floor, not equivalence — the header says so about CI and it is
# equally true here. A target whose commands are ALL trivial anchors to nothing
# and would pass this rung vacuously; that is exactly the `no gate` bucket, and
# it is pinned separately by EXPECTED_NO_GATE_TARGETS below.
#
# The reproducibility check below is the net for whatever anchoring does NOT
# erase: the root is probed TWICE and the two anchor lists must be identical. A
# future recipe embedding something per-invocation in a position anchors keep is
# then reported as text this oracle cannot judge, instead of arriving disguised
# as a decoupled closure.
#
# WHAT THE ORACLE DOES NOT SEE, both measured on this tree:
#
#   * ORDER. Prerequisites are a SET here, in the pin and in the oracle alike.
#     Nothing in this file distinguishes `check: test conformance-coverage` from
#     `check: conformance-coverage test`. Measured: REVERSING the whole `check:`
#     list leaves this guard's output identical as a set and changes only the
#     ORDER of its own `reached` lines, at exit 0. That is the correct verdict —
#     the real ordering lives in the `conformance-coverage: test` edge, not in
#     the list — but it is also the proof that the list's order is unpinned.
#
#   * EDGES between two targets that both stay in the closure. This is the
#     sharper form of the order gap and it is the one that matters on this
#     binding, so it is measured rather than reasoned about. Delete `test` from
#     `conformance-coverage:` and:
#
#       - this guard's entire output is byte-identical at exit 0 — membership
#         unchanged (10 targets), classification unchanged, and the ORACLE
#         PASSES, because `check:` still pulls `test` in directly, so every
#         anchor `conformance-coverage` has left is still in the root's list;
#       - `make -n check` still lists both the suite and the coverage script.
#
#     That edge is this binding's substitute for a completion marker: it has no
#     marker of its own, and `conformance-coverage: test` plus the coverage
#     guard's record count is what stands in for one. So which half actually
#     enforces it? Measured with the edge gone, over every way the coverage
#     guard can be reached — the question being whether it can ever read
#     stale-but-present evidence and pass:
#
#       local, fresh id per invocation      exit 2  "STALE — written by a
#                                                    different run"
#       CI-shaped, NEW job id, complete     exit 2  same; the stamp catches the
#         manifest from an EARLIER job               cross-run case
#       CI-shaped, SAME job id, manifest    exit 2  "only 0 distinct
#         stamped by the job's own                   conformance fixtures
#         truncation step, no suite run              replayed, expected >= 143"
#                                                    plus every area named
#       CI-shaped, SAME job id, PARTIAL     exit 2  "only 3 ... expected >= 143"
#         manifest                                   and "only 23 distinct
#                                                    scenarios, expected >= 151"
#       CI-shaped, SAME job id, COMPLETE    exit 0  correct — the suite really
#         manifest from an in-job suite              did run in that job
#       `make -j16 check`, 3 consecutive    exit 2  the guard reading a
#         runs (control with the edge                fresh-stamped but PARTIAL
#         restored: exit 0)                          manifest mid-ctest
#
#     So there is NO passing path on stale evidence. The stamp closes the
#     cross-run case and the record count closes the same-job-but-no-suite case,
#     and the two are jointly exhaustive over "a manifest that does not describe
#     a complete run". This revises the framing from last cycle: it was noted
#     then that inside a CI job the stamp proves same-job and not that the Test
#     step ran, which is true — rows three and four above are exactly that gap
#     exercised — but it cannot produce a false green on its own, because the
#     record count independently requires a complete suite's output.
#
#     Therefore the substitute is protected by EVIDENCE, not by this graph edge.
#     The edge buys ordering DETERMINISM — without it `make -j16 check` fails on
#     a race it is merely likely to lose — and nothing in this file protects the
#     edge itself. Do not read this pin as though it did.
#   * A recipe SWAPPED for another gate CI already runs. HALF of this is caught
#     now, by the anchor-collision rung below, and the two halves are worth
#     keeping apart because only one of them is still open:
#
#       caught    repointed at another CLOSURE MEMBER's gate.
#                 `test-interop-peer:` running check-wasm-corpus-column.sh was
#                 byte-identical to healthy at exit 0; the two own-anchor sets
#                 are now equal, so the collision rung names both and refuses.
#       OPEN      repointed at a ci.yml step NO member runs. Measured:
#                 `test-interop-peer:` running check-wasm-tiers.sh — a real step
#                 in the wasm job, which ci-reach.conf lists, so its anchor is
#                 reached — gives output byte-identical to healthy at exit 0
#                 with `make -n check` running the interop peer zero times. No
#                 collision, every count held, reach satisfied.
#
#     Closing the open half needs per-target recipe anchors: a second spelling
#     of every recipe inside this guard, which the header above records as a
#     mistake that already cost THIS binding a hardcoded duplicate path plus a
#     hand-written equality assertion. Out of scope on purpose, not covered by
#     accident. It also bounds the honest claim for this whole pin: the argument
#     is that a pin turns an invisible drop into a reviewable edit, and that
#     open half is an equally reviewable edit that stays equally undetected.
oracle_anchors() {
	dry_run "$1" | anchors | sort -u || true
}

oracle_anchors "$ROOT_TARGET" >"$oracle_root"
oracle_anchors "$ROOT_TARGET" >"$oracle_root_again"
if ! cmp -s "$oracle_root" "$oracle_root_again"; then
	echo "check-ci-reach: two identical \`$MAKE_BIN -n $ROOT_TARGET\` runs produced different anchors:" >&2
	# Only the differing lines, with no context: one unchanged neighbour here is
	# the whole clang-format invocation, ~130 paths on one line, which buries the
	# one line that actually moved.
	diff --unchanged-line-format= \
		--old-line-format='      probe 1: %L' \
		--new-line-format='      probe 2: %L' \
		"$oracle_root" "$oracle_root_again" >&2 || true
	echo "  The oracle below asks whether each closure target's commands appear in this" >&2
	echo "  list. That question is only answerable if the list is reproducible, so a" >&2
	echo "  recipe carrying anything minted per make invocation — a timestamp, a pid, a" >&2
	echo "  fresh id — in a position anchors do not erase has to be refused here rather" >&2
	echo "  than reported as a decoupled closure, which is the wrong diagnosis and the" >&2
	echo "  wrong fix. LAZILY_CONFORMANCE_RUN_ID is already erased by anchoring, because" >&2
	echo "  it only ever appears in a \`printf\`; whatever varies above does not, so either" >&2
	echo "  the recipe stops varying or this oracle cannot judge that target." >&2
	exit 1
fi
if [ ! -s "$oracle_root" ]; then
	echo "check-ci-reach: \`$MAKE_BIN -n $ROOT_TARGET\` yielded no anchors at all" >&2
	echo "  This fails closed further down — every target carrying a gate would report as" >&2
	echo "  decoupled, and the vacuity rung would fire behind that — but under the wrong" >&2
	echo "  name and pointing at the wrong file. An anchorless root is a root that runs" >&2
	echo "  no gate, not ten targets that stopped matching it." >&2
	exit 1
fi

ci_commands_scoped "${workflows[@]}" >"$ci_raw"

# Split the one scrape into its two products. The `\001runstep` marker lines are
# the step inventory; everything else is a command, anchored inside its own scope
# so a command cannot lend its anchors to a step that does not run it.
# `|| true` on the grep, and it is load-bearing (#lzgrepcpipefail again).
# `grep` exits 1 on a ZERO count, `set -o pipefail` propagates that, and `set -e`
# then kills the script — so a workflow set with no `run:` step at all exited 1
# having printed only the closure line, with the "empty haystack" message below
# never reached. Measured: pointing ci-reach.conf at a run:-less workflow gave
# exit 1 and one line of output. Fails closed, mutely, which is the wrong half of
# the bargain. The emptiness VERDICT belongs to the `-s` tests below.
{ grep -F $'\037\001runstep' <"$ci_raw" || true; } | sed -e $'s/\037\001runstep$//' | LC_ALL=C sort >"$ci_steps"
: >"$ci_scoped"
while IFS= read -r scoped_line; do
	[ -n "$scoped_line" ] || continue
	case "$scoped_line" in *$'\037\001runstep') continue ;; esac
	scope="${scoped_line%$'\037'*}"
	scoped_cmd="${scoped_line##*$'\037'}"
	# Prefixed by printf, not by sed: a step name is arbitrary text and every
	# sed delimiter, `&` and `\` in it would be substitution syntax.
	while IFS= read -r scoped_anchor; do
		[ -n "$scoped_anchor" ] || continue
		printf '%s\037%s\n' "$scope" "$scoped_anchor" >>"$ci_scoped"
	done < <(printf '%s\n' "$scoped_cmd" | anchors)
done <"$ci_raw"
cut -d$'\037' -f3 <"$ci_scoped" | LC_ALL=C sort -u >"$ci_anchor"

if [ ! -s "$ci_steps" ]; then
	echo "check-ci-reach: no run: steps found in ${workflows[*]} — a guard with an empty haystack passes everything" >&2
	exit 1
fi

if [ ! -s "$ci_anchor" ]; then
	echo "check-ci-reach: ${#workflows[@]} workflow(s) with run: steps, but not one checkable command in any of them" >&2
	echo "  Every run: body reduced to nothing — the anchor normalizer treats its whole" >&2
	echo "  program list as trivial. A guard with an empty haystack passes everything." >&2
	exit 1
fi


# ── validating the STEP PIN against both sides (#reversereachdirection) ────
#
# Three ways a pin can be worthless, refused here rather than discovered as a
# green run: it can be malformed (so it names no step), it can name a target
# that is not in the closure (so nothing consults it), and it can name a step
# that does not exist or is not unique (so the scope it selects is empty or
# wider than one step). An empty scope makes the reach check fail closed, which
# is the safe direction, but under the wrong name — the reader would be sent to
# the Makefile for a step that was renamed in the workflow.
pin_gate_targets=()
pin_gate_jobs=()
pin_gate_steps=()
pin_step_count="${#EXPECTED_GATE_STEPS[@]}"
pin_errors=""
for entry in "${EXPECTED_GATE_STEPS[@]}"; do
	entry_target="${entry%%|*}"
	entry_rest="${entry#*|}"
	entry_job="${entry_rest%%|*}"
	entry_step="${entry_rest#*|}"
	if [ "$entry_rest" = "$entry" ] || [ "$entry_step" = "$entry_rest" ] \
		|| [ -z "$entry_target" ] || [ -z "$entry_job" ] || [ -z "$entry_step" ]; then
		pin_errors="$pin_errors  - malformed entry '$entry' — expected exactly \`target|job|step name\`"$'\n'
		continue
	fi
	for existing in "${pin_gate_targets[@]:+${pin_gate_targets[@]}}"; do
		if [ "$existing" = "$entry_target" ]; then
			pin_errors="$pin_errors  - '$entry_target' is pinned to more than one step; a gate runs in one place"$'\n'
		fi
	done
	if ! in_closure "$entry_target"; then
		pin_errors="$pin_errors  - '$entry_target' is pinned to a step but is not in \`$MAKE_BIN $ROOT_TARGET\`'s closure — nothing consults this entry"$'\n'
	fi
	# Line-exact against the step inventory, and counted rather than tested:
	# zero occurrences is a renamed or deleted step, and two is a duplicate name
	# inside one job, which would widen the pinned scope back out to two steps.
	step_hits="$(awk -F'\037' -v j="$entry_job" -v st="$entry_step" \
		'$1 == j && $2 == st { n++ } END { print n + 0 }' "$ci_steps")"
	if [ "$step_hits" -ne 1 ]; then
		pin_errors="$pin_errors  - '$entry_target' is pinned to job '$entry_job' step '$entry_step', which matches $step_hits run: step(s) in ${workflows[*]} — it must match exactly 1"$'\n'
	fi
	pin_gate_targets+=("$entry_target")
	pin_gate_jobs+=("$entry_job")
	pin_gate_steps+=("$entry_step")
done

if [ -n "$pin_errors" ]; then
	echo "check-ci-reach: EXPECTED_GATE_STEPS does not describe this workflow:" >&2
	printf '%s' "$pin_errors" >&2
	echo "  Each entry pins the CI step that runs one gate, so reach can be asked inside" >&2
	echo "  that step instead of against every run: body at once. An entry that resolves" >&2
	echo "  to no step selects an EMPTY scope, which fails closed — safely, but under the" >&2
	echo "  wrong name, sending the reader to the Makefile for a step that moved in the" >&2
	echo "  workflow. Repin it, or, if the gate really left CI, say so with an excuse in" >&2
	echo "  $CONF." >&2
	exit 1
fi

printf 'pinned   %s gate step(s) = EXPECTED_GATE_STEPS\n' "$pin_step_count"

# Resolve a target's pinned step. Sets $GATE_STEP_JOB/$GATE_STEP_NAME and
# returns 0, or returns 1 for a target with no pin.
gate_step_for() {
	local t="$1" i
	GATE_STEP_JOB=""
	GATE_STEP_NAME=""
	for i in "${!pin_gate_targets[@]}"; do
		if [ "${pin_gate_targets[$i]}" = "$t" ]; then
			GATE_STEP_JOB="${pin_gate_jobs[$i]}"
			GATE_STEP_NAME="${pin_gate_steps[$i]}"
			return 0
		fi
	done
	return 1
}

# Does the PINNED STEP contain a command whose tokens contain this anchor as an
# in-order subsequence? Extra flags and arguments on the CI side are fine; missing
# ones are not.
#
# Scoped to one step, not to the whole workflow set (#reversereachdirection).
# The scope is what makes the question "does the step that is supposed to run
# this gate run it" instead of "does anything in CI happen to spell it", and the
# header on ci_commands_scoped records the three live supersets and the one live
# repoint that the unscoped form let through.
anchor_reached_in() {
	awk -v want="$1" -v scope_job="$2" -v scope_step="$3" -F'\037' '
		BEGIN { ANY = "\001any"; wn = split(want, w, / /) }
		$1 != scope_job || $2 != scope_step { next }
		{
			hn = split($3, h, / /)
			wi = 1
			# A wildcard on EITHER side matches, because either side may be the
			# one that spelled the argument through a variable.
			for (hi = 1; hi <= hn && wi <= wn; hi++)
				if (h[hi] == w[wi] || h[hi] == ANY || w[wi] == ANY) wi++
			if (wi > wn) { found = 1; exit }
		}
		END { exit found ? 0 : 1 }
	' "$ci_scoped"
}

# CI invoking the target through make counts as reach without any anchor work.
make_invokes() {
	awk -v target="$1" '
		{
			n = split($0, t, / /)
			if (t[1] != "make") next
			for (i = 2; i <= n; i++) if (t[i] == target) { found = 1; exit }
		}
		END { exit found ? 0 : 1 }
	' "$ci_anchor"
}

is_excused() {
	local t="$1" i
	for i in "${!excused_targets[@]}"; do
		[ "${excused_targets[$i]}" = "$t" ] && return 0
	done
	return 1
}

excuse_reason() {
	local t="$1" i
	for i in "${!excused_targets[@]}"; do
		if [ "${excused_targets[$i]}" = "$t" ]; then
			printf '%s' "${excused_reasons[$i]}"
			return
		fi
	done
}

unreadable=""
unreadable_count=0
decoupled=""
decoupled_count=0
unreached=""
unreached_count=0
stale=""
stale_count=0
nogate=""
nogate_count=0
unpinned=""
unpinned_count=0
mispinned=""
mispinned_count=0
scoped=()
scoped_count=0
reached=0
excused_ok=0

while IFS= read -r target; do
	[ -n "$target" ] || continue

	# ── probe 2 of 2: THIS target, ahead of the recipe read ───────────────
	#
	# Per target, not just for the root: `make -n check` can exit 0 while
	# `make -n <member>` exits 2 — an ordinary goal-conditional prerequisite
	# does exactly that — so a root-only probe never sees the member drop out.
	# Falsified separately: with only this verdict reverted, the
	# goal-conditional and excuse attacks both go green again at exit 0.
	#
	# Placed BEFORE the recipe read and BEFORE every classification below, with
	# a `continue`, so the target can appear as UNREADABLE and as nothing else.
	# Two laundering routes need that, and they are not the same one: an
	# unreadable recipe yields no anchors, so the emptiness test files it
	# `no gate`; and an `excuse:` line would have the conf vouch for it. On this
	# binding the measured line was `no gate`, never `excused`, because the
	# emptiness test already sat ahead of the excuse consultation — the excuse
	# never had to be believed for the gate to vanish. An excuse is a claim
	# about what CI RUNS, never a licence for a Makefile make cannot READ.
	#
	# Counted in its own bucket rather than appended to `unreached`: that list
	# prints under a "no CI run: step matches" heading and sends the reader to
	# the workflow file, which is the wrong place to look for a Makefile that
	# will not parse. The count still feeds the vacuity floor below.
	probe_rc=0
	"$MAKE_BIN" -n "$target" >/dev/null 2>"$probe_stderr" || probe_rc=$?
	if [ "$probe_rc" -ne 0 ]; then
		{
			printf '  - `%s -n %s` exited %s\n' "$MAKE_BIN" "$target" "$probe_rc"
			sed -e 's/^/      /' "$probe_stderr"
		} >>"$probe_failures"
		unreadable="$unreadable$target"$'\n'
		unreadable_count=$((unreadable_count + 1))
		printf 'UNREADABLE %-32s `%s -n %s` exited %s; its recipe cannot be read\n' "$target" "$MAKE_BIN" "$target" "$probe_rc"
		continue
	fi

	# ── the oracle, per target, ahead of the classification below ─────────
	#
	# The awk closure says this target is run by `make $ROOT_TARGET`. Make is
	# the only authority on whether that is true, so ask it: every anchor of
	# `make -n <target>` must appear in the root's anchor list. Extra anchors on
	# the root side are expected — it runs nine other gates.
	#
	# The full `make -n <target>` is anchored, prerequisites included, rather
	# than own_commands()' isolated slice. Deliberate, and the stricter
	# question: if the root really runs this target it runs the target's whole
	# subgraph, so every anchor belongs in the root's list. The isolated slice
	# is derived by a LINE COUNT (own_commands subtracts the prerequisites'
	# line count), which is the wrong instrument for a subset test.
	#
	# Own bucket with a `continue`, like UNREADABLE above: a target make does
	# not run has no honest reach verdict, and `reached` printed beside a
	# decoupled closure is the exact false comfort this rung exists to remove.
	oracle_anchors "$target" >"$oracle_target"
	oracle_missing=""
	oracle_missing_count=0
	while IFS= read -r oracle_line; do
		[ -n "$oracle_line" ] || continue
		if ! grep -qxF -e "$oracle_line" "$oracle_root"; then
			oracle_missing="$oracle_missing$oracle_line"$'\n'
			oracle_missing_count=$((oracle_missing_count + 1))
		fi
	done <"$oracle_target"
	if [ "$oracle_missing_count" -ne 0 ]; then
		# ── the diagnosis check, on the FAILURE PATH only ─────────────
		#
		# anchors() erases a volatile value in ONE of the two positions it
		# can occupy, not both. lazily-dart measured the pair: a LEADING
		# `VAR=$(RUN_ID) cmd` assignment is dropped by the normalizer and
		# this rung stays silent, but `cmd --tags run-$(RUN_ID)` keeps the
		# argument token verbatim and the rung exits 1 — correctly refusing,
		# and then BLAMING THE CLOSURE, which sends the reader to `check:`'s
		# prerequisite list to fix a recipe.
		#
		# So keep the refusal and repair the diagnosis: ask make the same
		# question twice. If the target does not answer the same way both
		# times, the recipe is non-deterministic and that is neither a
		# closure problem nor a CI problem.
		#
		# Deliberately on the failure path and not up front: it costs one
		# extra `make -n` only when a verdict is already going to be red, and
		# it cannot turn a green run red. The unconditional root re-probe
		# above catches the same class earlier for any target the root
		# actually runs — a volatile anchor in a coupled target reaches the
		# root's list too — so this arm is what covers the case the root
		# probe structurally cannot: a target whose own answer wobbles while
		# the root's stays still.
		#
		# NOT a loosening of the anchor comparison. Two bindings answered
		# this differently — py left it red with a do-not-loosen note, kt
		# pinned the run id for its own probes — and both keep the comparison
		# intact; this keeps it intact as well and only changes what the
		# refusal SAYS. On cpp no closure recipe passes a run id or manifest
		# positionally today (the run id appears only inside a `printf`,
		# which is trivial, and the manifest reaches ctest through a leading
		# `LAZILY_CONFORMANCE_MANIFEST=` assignment, which is dropped), so
		# this arm is a net for the next recipe rather than a live fix.
		oracle_anchors "$target" >"$oracle_target_again"
		if ! cmp -s "$oracle_target" "$oracle_target_again"; then
			echo "check-ci-reach: \`$MAKE_BIN -n $target\` answered differently twice:" >&2
			diff --unchanged-line-format= \
				--old-line-format='      probe 1: %L' \
				--new-line-format='      probe 2: %L' \
				"$oracle_target" "$oracle_target_again" >&2 || true
			echo "  This target's recipe is NON-DETERMINISTIC, so the oracle cannot judge it." >&2
			echo "  It is neither a closure problem nor a CI problem, and the rung below would" >&2
			echo "  have reported it as the former: something minted per make invocation — a" >&2
			echo "  timestamp, a pid, a fresh run id — sits in the command line in a position" >&2
			echo "  anchors keep, which is any ARGUMENT token." >&2
			echo "  Move it out of the command line: a leading \`VAR=value cmd\` assignment or a" >&2
			echo "  flag VALUE (\`--id=\$(ID)\`) is erased by the normalizer, a bare argument is" >&2
			echo "  not. Do NOT loosen the comparison to make this green." >&2
			exit 1
		fi
		{
			printf '  - %s: %s anchor(s) of its own are absent from `%s -n %s`\n' \
				"$target" "$oracle_missing_count" "$MAKE_BIN" "$ROOT_TARGET"
			while IFS= read -r oracle_line; do
				[ -n "$oracle_line" ] || continue
				printf '      %s\n' "$oracle_line"
			done <<<"$oracle_missing"
		} >>"$oracle_failures"
		decoupled="$decoupled$target"$'\n'
		decoupled_count=$((decoupled_count + 1))
		printf 'DECOUPLED %-31s in the Makefile-source closure, but `%s -n %s` does not run it\n' \
			"$target" "$MAKE_BIN" "$ROOT_TARGET"
		continue
	fi

	target_anchors="$(own_commands "$target" | anchors | sort -u || true)"

	if [ -z "$target_anchors" ]; then
		nogate="$nogate$target"$'\n'
		nogate_count=$((nogate_count + 1))
		continue
	fi

	# Recorded for the collision rung after the loop. Non-empty sets only: an
	# empty own-anchor set is the `no gate` bucket, which EXPECTED_NO_GATE_TARGETS
	# pins by name, and every member of it would otherwise "collide" with every
	# other one.
	printf '%s\t%s\n' "$(printf '%s' "$target_anchors" | tr '\n' '\037')" "$target" >>"$anchor_sets"

	hit=1
	missing_anchors=""
	scoped_job=""
	scoped_step=""
	if make_invokes "$target"; then
		# CI runs it through make, so there is no independent CI-side spelling
		# to scope. A pin for such a target asserts nothing — it would be
		# satisfied by the very `make <target>` line make_invokes() already
		# read — so it is a defect in the pin, collected and refused after the
		# loop rather than quietly honoured.
		if gate_step_for "$target"; then
			mispinned="$mispinned$target|$GATE_STEP_JOB|$GATE_STEP_NAME"$'\n'
			mispinned_count=$((mispinned_count + 1))
		fi
	else
		# ── the STEP PIN, ahead of the reach check (#reversereachdirection) ──
		#
		# Own bucket with a `continue`, like UNREADABLE and DECOUPLED above: a
		# target whose CI step is not pinned has no scope to ask reach in, and
		# an unscoped fallback here is exactly the flat question this rung
		# replaced. Counted in the accounting total below so it cannot leave
		# the audit uncounted.
		if ! gate_step_for "$target"; then
			unpinned="$unpinned$target"$'\n'
			unpinned_count=$((unpinned_count + 1))
			printf 'UNPINNED %-32s carries a gate CI must spell, but EXPECTED_GATE_STEPS names no step for it\n' "$target"
			continue
		fi
		scoped_job="$GATE_STEP_JOB"
		scoped_step="$GATE_STEP_NAME"
		scoped[$scoped_count]="$target"
		scoped_count=$((scoped_count + 1))
		while IFS= read -r a; do
			[ -n "$a" ] || continue
			if ! anchor_reached_in "$a" "$scoped_job" "$scoped_step"; then
				hit=0
				missing_anchors="$missing_anchors$a"$'\n'
			fi
		done <<<"$target_anchors"
	fi

	if is_excused "$target"; then
		if [ "$hit" -eq 1 ]; then
			stale="$stale$target"$'\n'
			stale_count=$((stale_count + 1))
		else
			excused_ok=$((excused_ok + 1))
			printf 'excused  %-32s %s\n' "$target" "$(excuse_reason "$target")"
		fi
		continue
	fi

	if [ "$hit" -eq 1 ]; then
		reached=$((reached + 1))
		printf 'reached  %s\n' "$target"
	else
		unreached="$unreached$target"$'\n'
		unreached_count=$((unreached_count + 1))
		printf 'MISSING  %s\n' "$target"
		while IFS= read -r a; do
			[ -n "$a" ] || continue
			printf '           job %s step `%s` does not run `%s`\n' \
				"$scoped_job" "$scoped_step" "$a"
		done <<<"$missing_anchors"
	fi
done <<<"$closure"

while IFS= read -r target; do
	[ -n "$target" ] || continue
	printf 'no gate  %-32s recipe runs no checkable command\n' "$target"
done <<<"$nogate"

# A recipe this guard could not LIST is a recipe it did not audit, and the
# `no gate` line printed for it above says the opposite (#lzgrepcpipefail).
# Refuse, with make's own stderr, and refuse HERE — ahead of the vacuity rung
# below and of the reached/unreached verdict, because both of those report the
# DOWNSTREAM symptom of an unlistable recipe. Measured before this rung existed:
# an unrunnable `$MAKE_BIN` emptied every command list and the vacuity rung
# blamed the Makefile's structure, which is the wrong subject and the wrong fix.
if [ -s "$probe_failures" ]; then
	echo >&2
	echo "check-ci-reach: \`$MAKE_BIN -n\` failed for the target(s) below, so their recipes could not be listed:" >&2
	cat "$probe_failures" >&2
	echo "  A recipe that cannot be listed looks exactly like one that runs no command," >&2
	echo "  and before this rung existed each one was filed 'no gate' — the one bucket" >&2
	echo "  the vacuity rung below does not count — so the audit shrank and still" >&2
	echo "  printed OK. They are counted as UNREADABLE now, and refused here." >&2
	exit 1
fi

# The awk-derived closure and the make-derived truth disagree
# (#pinreachclosure). Refused HERE, ahead of the reach verdict and the vacuity
# rung, for the same reason the unreadable rung above is: those two report on the
# closure, and this says the closure is not the thing `make $ROOT_TARGET` runs.
if [ -s "$oracle_failures" ]; then
	echo >&2
	echo "check-ci-reach: $decoupled_count closure target(s) that \`$MAKE_BIN -n $ROOT_TARGET\` does not run:" >&2
	cat "$oracle_failures" >&2
	echo "  The closure above is awk-scanned from Makefile SOURCE — the first \`^<target>:\`" >&2
	echo "  line, conditionals and all. Make parses those conditionals; this guard does" >&2
	echo "  not. So a target can sit in a \`^$ROOT_TARGET:\` line that no branch of an" >&2
	echo "  ifeq/ifdef actually selects, and every membership count above holds while the" >&2
	echo "  gate does not run. That is what this rung measured." >&2
	echo >&2
	echo "  Either put the target back on the branch make really takes, or, if the" >&2
	echo "  conditional is intended, give this guard a closure it can read: a single" >&2
	echo "  unconditional \`$ROOT_TARGET:\` line, with the optional part behind its own" >&2
	echo "  target. EXPECTED_CLOSURE_TARGETS cannot substitute for this — it is set-equal" >&2
	echo "  to the awk closure in both branches, which is exactly why this rung exists." >&2
	exit 1
fi

# ── ANCHOR COLLISIONS: the oracle's anti-weakening rung (#pinreachclosure) ──
#
# Normalization can only MERGE. anchors() drops flag values, reduces paths to
# basenames and erases trivial commands, so two different recipes can reduce to
# the same anchor set — and if two closure members do, every anchor rung above
# has been comparing a SMALLER set than it appears to, without saying so.
# lazily-dart named this and it is worth more here than anywhere: this closure
# invokes ctest, cmake and four separate guard scripts, so a collision is
# plausible in a way it is not in a binding whose members run one command each.
#
# Measured before trusting the oracle: all nine non-empty own-anchor sets in
# this closure are distinct, and eight of them are singletons naming a different
# program. dart's were distinct only down to two adjacent filenames.
#
# It also closes HALF of the recipe-swap attack the header above declares out of
# scope. Point one member's recipe at ANOTHER MEMBER's gate — measured earlier
# as `test-interop-peer:` running check-wasm-corpus-column.sh, byte-identical to
# healthy at exit 0 — and the two own-anchor sets become equal, so this rung
# names both targets and refuses. The remaining half is pointing a member at a
# real workflow step that no member runs; that still needs per-target recipe
# anchors and is still out of scope for the reason the header gives.
#
# OWN-recipe anchors, not the subgraph anchors the oracle compares. That is what
# makes the swap visible: `test-interop-peer`'s subgraph carries configure and
# build as well, so its subgraph set would never equal a leaf's.
if [ -s "$anchor_sets" ]; then
	collisions="$(LC_ALL=C sort "$anchor_sets" | awk -F'\t' '
		{ if ($1 == prev) { if (names == "") names = prevname; names = names " " $2 }
		  else { if (names != "") print names "\t" prev; names = ""; prev = $1; prevname = $2 } }
		END { if (names != "") print names "\t" prev }
	')"
	if [ -n "$collisions" ]; then
		echo >&2
		echo "check-ci-reach: closure members reduce to the SAME anchor set:" >&2
		while IFS=$'\t' read -r names joined; do
			[ -n "$names" ] || continue
			echo "  - $names" >&2
			printf '%s' "$joined" | tr '\037' '\n' | sed -e '/^$/d' -e 's/^/        /' >&2
		done <<<"$collisions"
		echo >&2
		echo "  Anchoring can only MERGE, so the rungs above have been comparing a smaller" >&2
		echo "  set than the target list suggests, and a gate whose recipe was repointed at" >&2
		echo "  another member's gate would be indistinguishable from the real thing." >&2
		echo "  Either the recipes really are duplicates — collapse them into one target —" >&2
		echo "  or one of them was repointed and should be restored. Do NOT resolve this by" >&2
		echo "  making the anchors coarser." >&2
		exit 1
	fi
fi

# ── the STEP PIN's two set rungs (#reversereachdirection) ─────────────────
#
# The pin is only worth what its agreement with the audit is worth, and it can
# disagree in three directions. One — a pin naming a step that does not exist —
# is refused up front, before any target is walked. The other two are only
# visible after the loop, and they are opposite mistakes:
#
#   UNPINNED    a member carries a gate CI has to spell and no entry names its
#               step. Without this rung the honest options are an unscoped
#               fallback, which is the flat question this whole pin replaces, or
#               an empty scope, which reddens under the wrong name.
#   pin unused  an entry exists for a target that never asked for it, because
#               the target is make-invoked, gateless, decoupled or unreadable.
#               That entry looks like coverage in the array and does no work —
#               the same shape as the excuse-for-a-non-member this file already
#               refuses (#pinreachclosure).
if [ "$unpinned_count" -ne 0 ]; then
	echo >&2
	echo "check-ci-reach: $unpinned_count target(s) carry a gate CI must spell, with no step pinned:" >&2
	while IFS= read -r t; do
		[ -n "$t" ] || continue
		echo "  - $t" >&2
	done <<<"$unpinned"
	echo "  Reach is asked INSIDE the pinned step, so an unpinned gate has no scope to" >&2
	echo "  ask it in. Add \`<target>|<job>|<step name>\` to EXPECTED_GATE_STEPS naming the" >&2
	echo "  CI step that runs it. Do NOT fall back to matching the whole workflow — that" >&2
	echo "  is the unscoped question this pin replaced, and the header on" >&2
	echo "  ci_commands_scoped records what it let through." >&2
	echo >&2
	echo "  Check the other cause first, because it is the likelier one and a new pin" >&2
	echo "  would paper over it: a target CI used to invoke THROUGH MAKE carries no pin by" >&2
	echo "  design, so deleting its \`make <target>\` step lands it here. Measured: removing" >&2
	echo "  the \`make fmt\` step reports \`fmt\` unpinned, and the fix is to restore the" >&2
	echo "  step, not to pin one." >&2
	exit 1
fi

if [ "$mispinned_count" -ne 0 ]; then
	echo >&2
	echo "check-ci-reach: $mispinned_count target(s) are reached THROUGH MAKE and also carry a step pin:" >&2
	while IFS= read -r t; do
		[ -n "$t" ] || continue
		echo "  - $t" >&2
	done <<<"$mispinned"
	echo "  CI invokes these by running \`make <target>\`, so they have no independent" >&2
	echo "  CI-side spelling and a pinned step asserts nothing about them: the step would" >&2
	echo "  be credited for saying \`make <target>\`, which is the fact make_invokes()" >&2
	echo "  already reads. Drop the entry. If the intent was to make CI spell the gate" >&2
	echo "  directly, change the WORKFLOW to run the command instead of the target, and" >&2
	echo "  the pin starts meaning something." >&2
	exit 1
fi


# ── the CLASSIFICATION pin (#pinreachclosure) ─────────────────────────────
#
# Membership and the oracle both hold when a target keeps its name, keeps its
# place in the graph, and has its RECIPE neutered — `test-interop-peer:` running
# `true`. `true` is on the trivial-program list at the top of this file (rightly:
# it carries no gate), so the target contributes no anchor, files under `no gate`,
# and is excused from CI reach by the one bucket that is excused by design.
# Measured on this tree: `OK — 8 target(s) reached by CI, 0 excused, 2 carrying
# no gate`, exit 0, with the membership pin and the accounting rung both silent
# because 10 targets still landed in 10 buckets.
#
# So the `no gate` bucket is pinned too, by the same set equality. It is the one
# bucket in this script that means "no CI obligation", which makes it the one
# bucket worth moving a gate into.
#
# Today it holds exactly the root: `check:`'s own recipe is `@echo "lazily-cpp:
# check OK"`, and `echo` is trivial. Everything else in the closure runs cmake,
# ctest, a script, or python3. A binding whose closure legitimately grows a
# mkdir-only reset step adds it here, in the same commit, with a reason.
EXPECTED_NO_GATE_TARGETS=(
  check
)

nogate_unpinned=""
nogate_unpinned_count=0
while IFS= read -r target; do
	[ -n "$target" ] || continue
	found=0
	for expected_target in "${EXPECTED_NO_GATE_TARGETS[@]}"; do
		if [ "$expected_target" = "$target" ]; then
			found=1
			break
		fi
	done
	if [ "$found" -eq 0 ]; then
		nogate_unpinned="$nogate_unpinned$target"$'\n'
		nogate_unpinned_count=$((nogate_unpinned_count + 1))
	fi
done <<<"$nogate"

# Line-exact, not a substring test: `*check\n*` against the `no gate` list also
# matches a target named `recheck`, which would report a pinned name as present
# because a different target happens to end in it.
nogate_missing=""
nogate_missing_count=0
for expected_target in "${EXPECTED_NO_GATE_TARGETS[@]}"; do
	found=0
	while IFS= read -r tt; do
		[ -n "$tt" ] || continue
		if [ "$tt" = "$expected_target" ]; then
			found=1
			break
		fi
	done <<<"$nogate"
	if [ "$found" -eq 0 ]; then
		nogate_missing="$nogate_missing$expected_target"$'\n'
		nogate_missing_count=$((nogate_missing_count + 1))
	fi
done

if [ "$((nogate_unpinned_count + nogate_missing_count))" -ne 0 ]; then
	echo >&2
	echo "check-ci-reach: the 'no gate' set does not equal EXPECTED_NO_GATE_TARGETS" >&2
	if [ "$nogate_unpinned_count" -gt 0 ]; then
		echo >&2
		echo "  $nogate_unpinned_count target(s) now carry NO checkable command and are not pinned as gateless:" >&2
		while IFS= read -r tt; do
			[ -n "$tt" ] || continue
			echo "    - $tt" >&2
		done <<<"$nogate_unpinned"
		echo "  A target whose recipe stopped running a gate keeps its name, keeps its place" >&2
		echo "  in the closure, and loses its CI obligation — 'no gate' is the one bucket" >&2
		echo "  this guard excuses by design. Restore the recipe, or, if the target really" >&2
		echo "  is bookkeeping now, pin it in EXPECTED_NO_GATE_TARGETS with a reason and" >&2
		echo "  say where the gate it used to run went." >&2
	fi
	if [ "$nogate_missing_count" -gt 0 ]; then
		echo >&2
		echo "  $nogate_missing_count target(s) pinned as gateless DO carry a gate now:" >&2
		while IFS= read -r tt; do
			[ -n "$tt" ] || continue
			echo "    - $tt" >&2
		done <<<"$nogate_missing"
		echo "  Good news needing a deliberate repin: drop it from EXPECTED_NO_GATE_TARGETS" >&2
		echo "  so its anchors start being required in CI, which is what the rest of this" >&2
		echo "  guard will then do." >&2
	fi
	exit 1
fi

# Ordered AFTER the 'no gate' classification pin above, deliberately. Both rungs
# see a member whose recipe was neutered to `true`, and they name different
# subjects: that one says the target stopped carrying a gate, this one says an
# entry in the pin array stopped doing work. The first is the cause and the
# second is its shadow, so the first must speak. Measured: with this rung placed
# ahead of it, `test-interop-peer: true` reported "1 EXPECTED_GATE_STEPS
# entry(ies) that no target consulted" and sent the reader to the array.
pin_unused=""
pin_unused_count=0
for expected_pin in "${pin_gate_targets[@]}"; do
	found=0
	for t in "${scoped[@]:+${scoped[@]}}"; do
		if [ "$t" = "$expected_pin" ]; then
			found=1
			break
		fi
	done
	if [ "$found" -eq 0 ]; then
		pin_unused="$pin_unused$expected_pin"$'\n'
		pin_unused_count=$((pin_unused_count + 1))
	fi
done
if [ "$pin_unused_count" -ne 0 ]; then
	echo >&2
	echo "check-ci-reach: $pin_unused_count EXPECTED_GATE_STEPS entry(ies) that no target consulted:" >&2
	while IFS= read -r t; do
		[ -n "$t" ] || continue
		echo "  - $t" >&2
	done <<<"$pin_unused"
	echo "  Each of these is in the closure, so the pin is not stale in the obvious way." >&2
	echo "  The target reached the loop and left it before the reach check: it is" >&2
	echo "  make-invoked, or its recipe carries no checkable command, or the oracle found" >&2
	echo "  it decoupled, or make could not read it. Those are reported above under their" >&2
	echo "  own names; this rung exists so the entry that stopped doing work does not sit" >&2
	echo "  in the array reading as coverage." >&2
	exit 1
fi

# A guard that examined nothing must not report OK — the same vacuity rule the
# conformance guards apply (#lzvacuousrun).
if [ "$((reached + excused_ok + unreached_count + unreadable_count + decoupled_count + unpinned_count))" -eq 0 ]; then
	echo "check-ci-reach: '$ROOT_TARGET' has no prerequisite target carrying a gate — nothing was verified" >&2
	exit 1
fi

# Every pinned target landed in exactly one bucket. The identity is structural
# today — the loop above walks $closure, which the pin proved equal to
# EXPECTED_CLOSURE_TARGETS — so this rung is not what catches a dropped gate.
# Measured, with only the set-equality verdict above neutered: it catches a
# PURE drop anyway (`accounted for 9 of 10`), because one target fewer in the
# closure is one bucket entry fewer here. It does NOT catch a SWAP — drop
# `test-interop-peer`, add `clean`, and this total holds at 10 while the reach
# audit stays silent because `clean` carries no gate; that case went green at
# exit 0 with the set-equality arm neutered and red with it intact. So the two
# rungs overlap on one attack and only the set-equality arm sees the other,
# which is the whole argument for equality over any count.
# What it catches is a future `continue` in that loop that skips a target without
# counting it anywhere, which is precisely the shape the unreadable-recipe hole
# had (#lzgrepcpipefail): a target left the audit through a bucket nobody
# totalled, and the guard reported OK over a smaller sample. A pin on the closure
# is only worth what the accounting under it is worth.
audited=$((reached + excused_ok + unreached_count + unreadable_count + decoupled_count + nogate_count + unpinned_count))
if [ "$audited" -ne "$pin_count" ]; then
	echo "check-ci-reach: accounted for $audited of $pin_count pinned closure target(s)" >&2
	echo "  reached=$reached excused=$excused_ok unreached=$unreached_count unreadable=$unreadable_count decoupled=$decoupled_count no-gate=$nogate_count unpinned=$unpinned_count" >&2
	echo "  The closure equals EXPECTED_CLOSURE_TARGETS, so every target should have" >&2
	echo "  landed in exactly one bucket above. One left the audit without being" >&2
	echo "  counted — that is a defect in this script, not in the Makefile." >&2
	exit 1
fi

status=0
if [ "$stale_count" -gt 0 ]; then
	echo >&2
	while IFS= read -r t; do
		[ -n "$t" ] || continue
		echo "check-ci-reach: '$t' is excused in $CONF but CI DOES reach it — remove the excuse" >&2
	done <<<"$stale"
	status=1
fi

if [ "$unreached_count" -gt 0 ]; then
	echo >&2
	echo "check-ci-reach: $unreached_count target(s) run by 'make $ROOT_TARGET' that no CI run: step reaches:" >&2
	while IFS= read -r t; do
		[ -n "$t" ] || continue
		echo "  - $t" >&2
	done <<<"$unreached"
	echo >&2
	echo "Add a CI step that runs it, or add an excuse with a reason to $CONF." >&2
	status=1
fi

if [ "$status" -eq 0 ]; then
	echo "check-ci-reach: OK — $reached target(s) reached by CI, $excused_ok excused, $nogate_count carrying no gate"
fi
exit "$status"
