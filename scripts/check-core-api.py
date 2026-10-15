#!/usr/bin/env python3
"""Validate the committed Core API snapshot against the profile and C++ surface."""

from __future__ import annotations

import argparse
import copy
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[1]
SNAPSHOT = ROOT / "api" / "core-api.json"
LIFECYCLE_KEYS = {
    "introduced",
    "deprecated_since",
    "replacement",
    "compatibility_window",
    "earliest_removal",
}


class CoreApiError(RuntimeError):
    pass


def load_json(path: Path) -> dict:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise CoreApiError(f"cannot read {path}: {error}") from error


def profile_binding(profile: dict) -> dict:
    for binding in profile.get("bindings", []):
        if binding.get("id") == "cpp":
            return binding
    raise CoreApiError("profile has no cpp binding")


def validate_lifecycle(symbol: dict) -> None:
    missing = LIFECYCLE_KEYS - symbol.keys()
    if missing:
        raise CoreApiError(
            f"{symbol.get('id', '<unknown>')}: missing lifecycle fields {sorted(missing)}"
        )
    introduced = symbol["introduced"]
    if not isinstance(introduced, int) or introduced < 1:
        raise CoreApiError(f"{symbol['id']}: introduced must be a positive generation")
    deprecated = symbol["deprecated_since"]
    if deprecated is None:
        if any(
            symbol[key] is not None
            for key in ("replacement", "compatibility_window", "earliest_removal")
        ):
            raise CoreApiError(
                f"{symbol['id']}: non-deprecated symbol has deprecation lifecycle data"
            )
        return
    if not isinstance(deprecated, int) or deprecated < introduced:
        raise CoreApiError(f"{symbol['id']}: invalid deprecated_since generation")
    if not isinstance(symbol["replacement"], str) or not symbol["replacement"]:
        raise CoreApiError(f"{symbol['id']}: deprecated symbol requires replacement")
    window = symbol["compatibility_window"]
    if not isinstance(window, dict):
        raise CoreApiError(f"{symbol['id']}: deprecated symbol requires compatibility_window")
    if window.get("minimum_cohorts", 0) < 2 or window.get("minimum_days", 0) < 90:
        raise CoreApiError(
            f"{symbol['id']}: compatibility window must be at least 2 cohorts and 90 days"
        )
    removal = symbol["earliest_removal"]
    if not isinstance(removal, int) or removal <= deprecated:
        raise CoreApiError(f"{symbol['id']}: earliest_removal must follow deprecation")


def validate_snapshot(snapshot: dict, profile: dict) -> None:
    binding = profile_binding(profile)
    if snapshot.get("binding") != "cpp":
        raise CoreApiError("snapshot binding must be cpp")
    if snapshot.get("core_api_generation") != profile.get("core_api_generation"):
        raise CoreApiError("snapshot core_api_generation differs from canonical profile")
    imports = binding.get("imports", [])
    if len(imports) != 1:
        raise CoreApiError("cpp profile must declare exactly one Core import")
    expected_import = {"kind": imports[0].get("kind"), "path": imports[0].get("path")}
    if snapshot.get("import") != expected_import:
        raise CoreApiError("snapshot import differs from canonical profile")

    abstract_items = profile.get("abstract_symbols", [])
    abstract = {item["id"]: item for item in abstract_items}
    mappings = binding.get("symbol_mappings", {})
    symbols = snapshot.get("symbols")
    if not isinstance(symbols, list):
        raise CoreApiError("snapshot symbols must be an array")
    by_id = {item.get("id"): item for item in symbols}
    if len(by_id) != len(symbols) or None in by_id:
        raise CoreApiError("snapshot symbol ids must be unique and non-empty")
    if set(by_id) != set(abstract) or set(by_id) != set(mappings):
        raise CoreApiError("snapshot symbol ids differ from canonical profile allow-list")
    snapshot_ids = [item["id"] for item in symbols]
    if snapshot_ids != [item["id"] for item in abstract_items]:
        raise CoreApiError("snapshot symbols are not in canonical normalized order")

    support = binding.get("support_range", {})
    generation = snapshot["core_api_generation"]
    if not (
        support.get("minimum_core_api_generation")
        <= generation
        <= support.get("maximum_core_api_generation")
    ):
        raise CoreApiError("snapshot generation is outside the cpp support range")

    for symbol_id, symbol in by_id.items():
        validate_lifecycle(symbol)
        canonical = abstract[symbol_id]
        mapping = mappings[symbol_id]
        if symbol.get("kind") != canonical.get("kind"):
            raise CoreApiError(f"{symbol_id}: kind differs from canonical profile")
        if symbol.get("public_names") != mapping.get("public_names"):
            raise CoreApiError(f"{symbol_id}: public_names differ from canonical profile")
        for key in ("introduced", "deprecated_since", "replacement", "earliest_removal"):
            if symbol.get(key) != canonical.get(key):
                raise CoreApiError(f"{symbol_id}: {key} differs from canonical profile")


def compile_probe(
    snapshot: dict, compiler: str, include_dir: Path, language_standard: int
) -> None:
    symbols = {item["id"]: item for item in snapshot["symbols"]}
    required = {"context", "source", "computed", "effect", "batch", "scope", "typed_context"}
    if set(symbols) != required:
        raise CoreApiError("C++ compile probe only supports the complete core-v1 allow-list")

    context = symbols["context"]["public_names"][0]
    source = symbols["source"]["public_names"][0]
    computed = symbols["computed"]["public_names"][0]
    effect = symbols["effect"]["public_names"][0]
    teardown = symbols["scope"]["public_names"][1]
    batch_method = symbols["batch"]["public_names"][0].rsplit("::", 1)[-1]
    scope_method = symbols["scope"]["public_names"][0].rsplit("::", 1)[-1]
    typed = symbols["typed_context"]["public_names"][0]
    header = snapshot["import"]["path"]
    source_text = f"""#include <{header}>
#include <type_traits>

using CoreContext = {context};
using CoreSource = {source}<int>;
using CoreComputed = {computed}<int>;
using CoreEffect = {effect};
using CoreTeardownScope = {teardown};
using CoreTypedContext = {typed};

static_assert(std::is_class_v<CoreContext>);
static_assert(std::is_class_v<CoreSource>);
static_assert(std::is_class_v<CoreComputed>);
static_assert(std::is_class_v<CoreEffect>);
static_assert(std::is_class_v<CoreTeardownScope>);
static_assert(std::is_same_v<CoreTypedContext, CoreContext>);

void core_api_probe(CoreContext& context) {{
  static_assert(std::is_same_v<decltype(context.source(1)), CoreSource>);
  CoreSource source_handle = context.source(1);
  CoreComputed computed_handle = context.computed<int>(
      [source_handle](auto& compute) {{ return source_handle.get(compute) + 1; }});
  CoreEffect effect_handle = context.effect_void(
      [computed_handle](auto& compute) {{ (void)computed_handle.get(compute); }});
  context.{batch_method}([](CoreContext&) {{}});
  CoreTeardownScope scope = context.{scope_method}();
  CoreSource scoped_source = scope.source(2);
  context.dispose_effect(effect_handle);
  (void)scoped_source;
}}
"""
    with tempfile.TemporaryDirectory(prefix="lazily-core-api-") as directory:
        probe = Path(directory) / "core_api_probe.cpp"
        probe.write_text(source_text, encoding="utf-8")
        try:
            result = subprocess.run(
                [
                    compiler,
                    f"-std=c++{language_standard}",
                    "-fsyntax-only",
                    "-I",
                    str(include_dir),
                    str(probe),
                ],
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                check=False,
            )
        except OSError as error:
            raise CoreApiError(f"cannot execute C++ compiler {compiler}: {error}") from error
    if result.returncode != 0:
        raise CoreApiError(
            f"Core compile probe failed for C++{language_standard}:\n{result.stdout}"
        )


def check(snapshot: dict, profile: dict, compiler: str, include_dir: Path) -> None:
    validate_snapshot(snapshot, profile)
    toolchains = profile_binding(profile).get("toolchains", {})
    try:
        standards = {
            int(toolchains["minimum"]["version"]),
            int(toolchains["current"]["version"]),
        }
    except (KeyError, TypeError, ValueError) as error:
        raise CoreApiError("cpp profile has invalid minimum/current C++ standards") from error
    for standard in sorted(standards):
        compile_probe(snapshot, compiler, include_dir, standard)


def self_test(snapshot: dict, profile: dict, compiler: str, include_dir: Path) -> None:
    mutations: list[tuple[str, dict, dict, str]] = []

    removed = copy.deepcopy(snapshot)
    removed["symbols"].pop(0)
    mutations.append(
        ("removed released symbol", removed, profile, "allow-list")
    )

    additive_profile = copy.deepcopy(profile)
    additive_profile["abstract_symbols"].append(
        {
            "id": "future_core_symbol",
            "kind": "type",
            "introduced": 2,
            "deprecated_since": None,
            "replacement": None,
            "earliest_removal": None,
        }
    )
    profile_binding(additive_profile)["symbol_mappings"]["future_core_symbol"] = {
        "import_id": "core_header",
        "public_names": ["lazily::FutureCoreSymbol"],
    }
    mutations.append(
        ("unapproved additive profile symbol", snapshot, additive_profile, "allow-list")
    )

    generation = copy.deepcopy(snapshot)
    generation["core_api_generation"] += 1
    mutations.append(
        ("unapproved generation change", generation, profile, "core_api_generation")
    )

    lifecycle = copy.deepcopy(snapshot)
    lifecycle["symbols"][0]["deprecated_since"] = 1
    mutations.append(
        ("incomplete deprecation metadata", lifecycle, profile, "requires replacement")
    )

    missing_surface = copy.deepcopy(snapshot)
    missing_profile = copy.deepcopy(profile)
    missing_surface["symbols"][0]["public_names"] = ["lazily::MissingContext"]
    profile_binding(missing_profile)["symbol_mappings"]["context"]["public_names"] = [
        "lazily::MissingContext"
    ]
    mutations.append(
        (
            "missing public header symbol",
            missing_surface,
            missing_profile,
            "compile probe failed",
        )
    )

    for label, mutation, mutation_profile, expected_error in mutations:
        try:
            check(mutation, mutation_profile, compiler, include_dir)
        except CoreApiError as error:
            if expected_error in str(error):
                continue
            raise CoreApiError(
                f"negative self-test for {label} hit the wrong gate: {error}"
            ) from error
        raise CoreApiError(f"negative self-test did not reject {label}")
    print(f"core API negative self-test: {len(mutations)} mutations rejected")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--profile", required=True, type=Path)
    parser.add_argument("--snapshot", type=Path, default=SNAPSHOT)
    parser.add_argument("--compiler", default=os.environ.get("CXX", "c++"))
    parser.add_argument("--include-dir", type=Path, default=ROOT / "include")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    try:
        snapshot = load_json(args.snapshot)
        profile = load_json(args.profile)
        check(snapshot, profile, args.compiler, args.include_dir)
        if args.self_test:
            self_test(snapshot, profile, args.compiler, args.include_dir)
        else:
            print("core API snapshot: cpp generation 1 OK")
    except CoreApiError as error:
        print(f"core API check failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
