#!/usr/bin/env bash
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

mkdir -p "$tmp/lazily"
if [ -n "${LAZILY_CORE_ARTIFACT_VERSION:-}" ]; then
  : "${LAZILY_CORE_ARTIFACT_SHA256:?LAZILY_CORE_ARTIFACT_SHA256 is required for a published source archive}"
  archive="$tmp/lazily-cpp-v$LAZILY_CORE_ARTIFACT_VERSION.tar.gz"
  curl --fail --location --silent --show-error \
    "https://github.com/lazily-hub/lazily-cpp/archive/refs/tags/v$LAZILY_CORE_ARTIFACT_VERSION.tar.gz" \
    --output "$archive"
  printf '%s  %s\n' "$LAZILY_CORE_ARTIFACT_SHA256" "$archive" | sha256sum --check --status
  tar -xzf "$archive" --strip-components=1 -C "$tmp/lazily"
else
  (cd "$root" && tar -czf "$tmp/lazily-cpp.tar.gz" CMakeLists.txt include LICENSE NOTICE README.md)
  tar -xzf "$tmp/lazily-cpp.tar.gz" -C "$tmp/lazily"
fi
mkdir -p "$tmp/consumer"
cp "$root/tests/core-artifact-consumer/CMakeLists.txt" "$tmp/consumer/"
cp "$root/tests/core-artifact-consumer/main.cpp" "$tmp/consumer/"
cmake -S "$tmp/consumer" -B "$tmp/consumer/build" \
  -DLAZILY_ARTIFACT="$tmp/lazily" \
  -DLAZILY_CXX_STANDARD="${LAZILY_CXX_STANDARD:-17}"
cmake --build "$tmp/consumer/build" --parallel
"$tmp/consumer/build/lazily_core_consumer"

if grep -R -F "$root" "$tmp/consumer/build" >/dev/null 2>&1; then
  echo "consumer metadata references the source checkout" >&2
  exit 1
fi
