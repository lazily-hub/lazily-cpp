#!/usr/bin/env python3
"""Build and invoke a C consumer of the shipped lazily_ffi shared library."""

from __future__ import annotations

import json
import os
import subprocess
import sys
from pathlib import Path


def run(argv: list[str]) -> None:
    completed = subprocess.run(argv, text=True, check=False)
    if completed.returncode != 0:
        raise RuntimeError(f"{' '.join(argv)} exited {completed.returncode}")


def main() -> None:
    evidence_raw = os.environ.get("LAZILY_CORE_EVIDENCE_PATH")
    if not evidence_raw:
        raise RuntimeError("LAZILY_CORE_EVIDENCE_PATH is required")
    evidence = Path(evidence_raw).resolve()
    root = Path(__file__).resolve().parents[1]
    build = root / "build/core-conformance-ffi"
    build.mkdir(parents=True, exist_ok=True)
    run([
        "cmake", "-S", str(root), "-B", str(build),
        "-DLAZILY_BUILD_TESTS=OFF", "-DLAZILY_BUILD_FFI=ON", "-DCMAKE_BUILD_TYPE=Release",
    ])
    run(["cmake", "--build", str(build), "--target", "lazily_ffi", "--parallel"])
    library = build / "liblazily_ffi.so"
    if not library.is_file():
        raise RuntimeError(f"shared library was not built: {library}")
    consumer = build / "core-ffi-smoke"
    run([
        os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
        f"-I{root / 'include'}", str(root / "tests/native/core_ffi_smoke.c"),
        f"-L{build}", "-llazily_ffi", f"-Wl,-rpath,{build}", "-o", str(consumer),
    ])
    run([str(consumer)])
    evidence.parent.mkdir(parents=True, exist_ok=True)
    evidence.write_text(json.dumps({
        "schema_version": 1,
        "claim_id": os.environ.get("LAZILY_CORE_CLAIM_ID"),
        "status": "passed",
        "artifact": str(library.relative_to(root)),
        "consumer": str(consumer.relative_to(root)),
        "invoked": True,
    }, indent=2, sort_keys=True) + "\n", encoding="utf-8")


if __name__ == "__main__":
    try:
        main()
    except Exception as error:
        print(f"Core C-ABI proof FAILED: {error}", file=sys.stderr)
        raise SystemExit(1)
