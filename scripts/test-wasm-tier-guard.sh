#!/usr/bin/env bash
# Toolchain-free regression probes for check-wasm-tiers.sh (#lazilycppcheck).

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"

fail() {
  echo "test-wasm-tier-guard: $*" >&2
  exit 1
}

scratch_dir="$(mktemp -d "${TMPDIR:-/tmp}/lazily-cpp-wasm-tier-guard.XXXXXX")"
trap 'rm -rf "$scratch_dir"' EXIT
manifest="$scratch_dir/manifest.txt"

printf '%s\n' \
  '# generated manifest metadata' \
  '@replayed\tfamily/fixture.json\tscenario' \
  '' \
  'family/fixture.json' \
  'family/fixture.json' > "$manifest"
filtered="$(bash ./scripts/check-wasm-tiers.sh --fixtures-of "$manifest")"
[[ "$filtered" == "family/fixture.json" ]] || fail \
  "fixture filter retained metadata/ledger lines: $(printf '%q' "$filtered")"

# A PATH-local grep simulates a deterministic reader failure even when the test
# process is root (where chmod 000 can still be readable).
real_grep="$(command -v grep)"
mkdir -p "$scratch_dir/bin"
cat > "$scratch_dir/bin/grep" <<'GREPPROBE'
#!/usr/bin/env bash
last=""
for last in "$@"; do :; done
if [[ "$last" == "$WASM_TEST_UNREADABLE" ]]; then
  echo "grep: $last: simulated read error" >&2
  exit 2
fi
exec "$WASM_TEST_REAL_GREP" "$@"
GREPPROBE
chmod +x "$scratch_dir/bin/grep"

if unreadable="$(PATH="$scratch_dir/bin:$PATH" \
    WASM_TEST_REAL_GREP="$real_grep" WASM_TEST_UNREADABLE="$manifest" \
    bash ./scripts/check-wasm-tiers.sh --require-manifest "$manifest" core 1 2>&1)"; then
  fail "simulated unreadable manifest unexpectedly passed"
fi
case "$unreadable" in
  *"could not be read"*"unreadable evidence is not an empty tier"*) ;;
  *) fail "unreadable manifest did not receive its dedicated diagnostic: $unreadable" ;;
esac
case "$unreadable" in
  *"replayed 0"*) fail "unreadable manifest was folded into the empty-tier diagnostic" ;;
esac

configured_dir="$scratch_dir/configured-core"
mkdir -p "$configured_dir"
if unbuilt="$(bash ./scripts/check-wasm-tiers.sh \
    --check-tier-targets core "$configured_dir" 2>&1)"; then
  fail "configured-but-unbuilt tier unexpectedly passed"
fi
case "$unbuilt" in
  *"tests directory is missing"*"configured but not built"*) ;;
  *) fail "configured-but-unbuilt tier failed without its dedicated diagnostic: $unbuilt" ;;
esac

echo "test-wasm-tier-guard: OK"
