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

# Command lines from every `run:` step. Comment lines inside a run body are
# stripped here — the whole reason this guard is a script.
ci_commands() {
	awk '
		function flush() { if (buf != "") { print buf; buf = "" } }
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
					if (buf != "") { print buf " " line; buf = "" } else print line
					next
				}
			}

			if (line ~ /^[[:space:]]*(-[[:space:]]+)?run:[[:space:]]*[|>][-+]?[[:space:]]*$/) {
				inblock = 1
				block_indent = indent
				buf = ""
				next
			}
			if (line ~ /^[[:space:]]*(-[[:space:]]+)?run:[[:space:]]*[^|>[:space:]]/) {
				sub(/^[[:space:]]*(-[[:space:]]+)?run:[[:space:]]*/, "", line)
				print line
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
# The `make -n` probes' scratch: one file collecting every failure for the final
# report, one for the stderr of the probe in flight.
probe_failures="$(mktemp)"
probe_stderr="$(mktemp)"
# The oracle's scratch: the ROOT's real anchor list, a second copy of it for the
# reproducibility check, the target in flight, and the accumulated findings.
oracle_root="$(mktemp)"
oracle_root_again="$(mktemp)"
oracle_target="$(mktemp)"
oracle_failures="$(mktemp)"
trap 'rm -f "$ci_raw" "$ci_anchor" "$probe_failures" "$probe_stderr" "$oracle_root" "$oracle_root_again" "$oracle_target" "$oracle_failures"' EXIT

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
#     `check: conformance-coverage test`.
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
#     guard's record count is what stands in for one. So of those two halves,
#     the RECORD COUNT is the half anything enforces. Measured with the edge
#     gone: `make conformance-coverage` alone exits 2 on the run-id stamp
#     ("stale or unstamped evidence"), and `make -j16 check` exited 2 on three
#     consecutive runs with "only 3 distinct conformance fixtures replayed,
#     expected >= 143" — the coverage guard reading a fresh-stamped but PARTIAL
#     manifest while ctest was still running (control: edge restored, `make -j16
#     check` exits 0). So it fails CLOSED, but it fails closed by losing a RACE,
#     not by a rule. The edge is what makes that race not exist. Nothing in this
#     file protects it, and this pin should not be read as though it did.
#   * A recipe SWAPPED for another gate CI already runs (js's Attack 4:
#     `test-interop-peer:` running the conformance script instead of the peer).
#     Every count holds, the anchors still match a real CI step, the verdict is
#     byte-identical. Closing it needs per-target recipe anchors — a second
#     spelling of every recipe inside this guard — which the header above
#     records as a mistake that already cost THIS binding a hardcoded duplicate
#     path plus a hand-written equality assertion. Out of scope on purpose, not
#     covered by accident. It also bounds the honest claim for this whole pin:
#     the argument is that it turns an invisible drop into a reviewable edit,
#     and Attack 4 is an equally reviewable edit that stays equally undetected.
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

ci_commands "${workflows[@]}" >"$ci_raw"
anchors <"$ci_raw" | sort -u >"$ci_anchor"

if [ ! -s "$ci_anchor" ]; then
	echo "check-ci-reach: no run: steps found in ${workflows[*]} — a guard with an empty haystack passes everything" >&2
	exit 1
fi

# Does CI contain a command whose tokens contain this anchor as an in-order
# subsequence? Extra flags and arguments on the CI side are fine; missing ones are
# not.
anchor_reached() {
	awk -v want="$1" '
		BEGIN { ANY = "\001any"; wn = split(want, w, / /) }
		{
			hn = split($0, h, / /)
			wi = 1
			# A wildcard on EITHER side matches, because either side may be the
			# one that spelled the argument through a variable.
			for (hi = 1; hi <= hn && wi <= wn; hi++)
				if (h[hi] == w[wi] || h[hi] == ANY || w[wi] == ANY) wi++
			if (wi > wn) { found = 1; exit }
		}
		END { exit found ? 0 : 1 }
	' "$ci_anchor"
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

	hit=1
	missing_anchors=""
	if ! make_invokes "$target"; then
		while IFS= read -r a; do
			[ -n "$a" ] || continue
			if ! anchor_reached "$a"; then
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
			printf '           no CI run: step matches `%s`\n' "$a"
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

# A guard that examined nothing must not report OK — the same vacuity rule the
# conformance guards apply (#lzvacuousrun).
if [ "$((reached + excused_ok + unreached_count + unreadable_count + decoupled_count))" -eq 0 ]; then
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
audited=$((reached + excused_ok + unreached_count + unreadable_count + decoupled_count + nogate_count))
if [ "$audited" -ne "$pin_count" ]; then
	echo "check-ci-reach: accounted for $audited of $pin_count pinned closure target(s)" >&2
	echo "  reached=$reached excused=$excused_ok unreached=$unreached_count unreadable=$unreadable_count decoupled=$decoupled_count no-gate=$nogate_count" >&2
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
