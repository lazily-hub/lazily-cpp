#!/usr/bin/env python3
"""Prove the focused stale-handle test kills a missing-generation-check mutant."""

from __future__ import annotations

import argparse
import shutil
import subprocess
import tempfile
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--source-root", type=Path, required=True)
    parser.add_argument("--build-root", type=Path, required=True)
    args = parser.parse_args()

    args.build_root.mkdir(parents=True, exist_ok=True)
    mutations = (
        (
            "source",
            "  template <typename T, typename M> void dispose_cell(const Source<T, M>& handle) {\n"
            "    if (node_generation(handle.id()) != handle.generation()) return;\n"
            "    dispose_cell_id(handle.id());\n"
            "  }",
            "  template <typename T, typename M> void dispose_cell(const Source<T, M>& handle) {\n"
            "    dispose_cell_id(handle.id());\n"
            "  }",
            "stale source removed its successor",
        ),
        (
            "computed",
            "  template <typename T> void dispose_slot(const Computed<T>& handle) {\n"
            "    if (node_generation(handle.id()) != handle.generation()) return;\n"
            "    dispose_slot_id(handle.id());\n"
            "  }",
            "  template <typename T> void dispose_slot(const Computed<T>& handle) {\n"
            "    dispose_slot_id(handle.id());\n"
            "  }",
            "stale computed removed its successor",
        ),
        (
            "effect",
            "      if (node_generation(handle.id) != handle.generation) return;\n",
            "",
            "stale effect removed its successor",
        ),
    )

    with tempfile.TemporaryDirectory(prefix="stale-handle-mutant-", dir=args.build_root) as raw:
        temporary = Path(raw)
        original = (args.source_root / "include" / "lazily" / "context.hpp").read_text(
            encoding="utf-8"
        )
        for name, needle, replacement, expected_failure in mutations:
            if original.count(needle) != 1:
                raise RuntimeError(
                    f"{name} stale-handle generation guard drifted; mutation was not applied"
                )

            mutant_root = temporary / name
            include = mutant_root / "include"
            shutil.copytree(args.source_root / "include", include)
            context = include / "lazily" / "context.hpp"
            context.write_text(original.replace(needle, replacement), encoding="utf-8")

            binary = mutant_root / "stale-handle-mutant"
            compile_result = subprocess.run(
                [
                    args.compiler,
                    "-std=c++17",
                    "-O0",
                    f"-I{include}",
                    str(args.source_root / "tests" / "test_stale_handle_disposal.cpp"),
                    "-o",
                    str(binary),
                ],
                text=True,
                capture_output=True,
            )
            if compile_result.returncode != 0:
                raise RuntimeError(
                    f"{name} stale-handle mutant did not compile:\n"
                    + compile_result.stdout
                    + compile_result.stderr
                )

            mutant = subprocess.run([str(binary)], text=True, capture_output=True)
            if mutant.returncode == 0:
                raise RuntimeError(
                    f"{name} generation-check mutant survived the stale-handle regression test"
                )
            if expected_failure not in mutant.stderr:
                raise RuntimeError(
                    f"{name} generation-check mutant failed for an unexpected reason:\n"
                    + mutant.stdout
                    + mutant.stderr
                )

    print("stale-handle mutation check: source, computed, and effect fences rejected")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
