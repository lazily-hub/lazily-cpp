#!/usr/bin/env bash
# Regression probes for the two normalizer/diagnostic failures recorded by
# #twopreexisting. Kept separate from the large guard so each probe can feed the
# real implementation a deliberately malformed input and assert its failure.

set -euo pipefail

fail() {
	echo "test-ci-reach-quirks: $*" >&2
	exit 1
}

normalized="$({
	printf '%s\n' 'command -v uvx >/dev/null 2>&1'
	printf '%s\n' 'python3 tools/check.py 2>>logs/check.log'
	printf '%s\n' 'runner <fixtures/input.txt'
} | bash ./scripts/check-ci-reach.sh --anchors-stdin)"
expected="$(printf '%s\n' 'command -v uvx' 'python3 check.py' 'runner')"
case "$normalized" in
	*"$expected") ;;
	*) fail "normalizer did not emit the expected anchor suffix: $(printf '%q' "$normalized")" ;;
esac
case "$normalized" in
	*null*) fail "attached redirection targets leaked into anchors: $(printf '%q' "$normalized")" ;;
esac

coverage_guard="$(<./scripts/check-conformance-coverage.sh)"
case "$coverage_guard" in
	*"builds, runs ctest and"*)
		fail "stale-manifest diagnostic still makes the unconditional build/ctest claim"
		;;
esac
case "$coverage_guard" in
	*'`conformance-coverage: test` prerequisite'*) ;;
	*) fail "stale-manifest diagnostic does not identify the required Makefile edge" ;;
esac

# The guidance is true only while this edge exists. Pinning both sides here turns
# a future graph edit into a focused failure instead of stale operator advice.
makefile="$(<./Makefile)"
case "$makefile" in
	*$'\nconformance-coverage: test\n'*) ;;
	*) fail "Makefile no longer carries the edge named by the stale-manifest diagnostic" ;;
esac

echo "test-ci-reach-quirks: OK"
