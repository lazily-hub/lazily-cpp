#!/usr/bin/env bash
set -euo pipefail

cmake_bin="${CMAKE:-cmake}"
build_dir="${BUILD_DIR:-build}"

"$cmake_bin" -S . -B "$build_dir" \
  -DLAZILY_BUILD_TESTS=ON \
  -DLAZILY_BUILD_BENCHMARKS=OFF
"$cmake_bin" --build "$build_dir"
"$cmake_bin" \
  -DLAZILY_BUILD_DIR="$build_dir" \
  -DLAZILY_PACKAGE_DIR="$build_dir/package-content" \
  -P tests/check_package_content.cmake
