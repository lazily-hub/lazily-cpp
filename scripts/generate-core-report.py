#!/usr/bin/env python3
"""Run the Core fixture consumer and emit its observed qualification report."""

from __future__ import annotations

import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


BINDING_ID = "cpp"
SKIP_FIXTURE = "reactive-graph/feedback_drain_bound_reports_exhaustion.json"


def require_env(name: str) -> Path:
    value = os.environ.get(name)
    if not value:
        raise RuntimeError(f"{name} is required")
    return Path(value).resolve()


def run(argv: list[str], *, env: dict[str, str] | None = None) -> None:
    completed = subprocess.run(argv, env=env, text=True, check=False)
    if completed.returncode != 0:
        raise RuntimeError(f"{' '.join(argv)} exited {completed.returncode}")


# A mutant only counts as killed when the runner rejected it through its divergence
# assertion: the library's observed value disagreed with the mutated expectation
# (`check`/`check_strs`: "observed X, expected Y"), or the two scenarios named by
# `observationally_equal` produced different library observations. Any other
# non-zero exit proves the runner READ the leaf, not that the assertion over it
# discriminates: renaming the scenario label in
# scope_teardown_equals_fold_of_disposals.json#/expected aborts on the runner's
# own `REQUIRE(... "observationally_equal names a scenario that was not
# replayed")`, which also prints "FAIL:", and a fixture-set drift or a missing
# fixture does the same. Such a mutant is recorded and the next leaf is tried.
FINDING_SUFFIX = " \u2014 reactive-graph conformance FINDING"
DIVERGENCE_LINE = re.compile(
    r"^FAIL: (?P<fixture>[^\s/]+\.json)(?:/\S+)? "
    r"(?:step \d+ \S+: observed .*, expected .*"
    r"|scenarios '[^']*' and '[^']*' are not observationally equal)"
    + re.escape(FINDING_SUFFIX),
    re.MULTILINE,
)
MAX_MUTANTS_PER_ROUTE = 8


def rejected_by(output: str, fixture_path: str) -> str:
    """'divergence' only for a library-divergence FINDING on the mutated fixture."""
    fixture_name = Path(fixture_path).name
    for match in DIVERGENCE_LINE.finditer(output):
        if match.group("fixture") == fixture_name:
            return "divergence"
    return "other_failure"


def run_negative_control(argv: list[str], *, env: dict[str, str], fixture_path: str) -> dict[str, object]:
    completed = subprocess.run(
        argv,
        env=env,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
    )
    if completed.returncode == 0:
        raise RuntimeError("mutated assertion was accepted by the reactive-graph runner")
    output = completed.stdout or ""
    fail_at = output.find("FAIL: ")
    return {
        "status": "rejected",
        "exit_code": completed.returncode,
        "rejected_by": rejected_by(output, fixture_path),
        "output_excerpt": output[fail_at:fail_at + 1000] if fail_at >= 0 else output[-1000:],
    }


def pointer_token(value: str) -> str:
    return value.replace("~", "~0").replace("/", "~1")


def route_pattern(pointer: str) -> re.Pattern[str]:
    parts = pointer.removeprefix("/").split("/")
    pattern = ""
    for part in parts:
        if part == "*":
            pattern += r"\[\d+\]"
        else:
            if pattern:
                pattern += r"\."
            pattern += re.escape(part.replace("~1", "/").replace("~0", "~"))
    return re.compile(r"^" + pattern + r"$")


def pointer_matches(document: object, pointer: str) -> list[tuple[object, str]]:
    matches: list[tuple[object, str]] = [(document, "")]
    for encoded in pointer.removeprefix("/").split("/"):
        token = encoded.replace("~1", "/").replace("~0", "~")
        expanded: list[tuple[object, str]] = []
        for value, path in matches:
            if token == "*" and isinstance(value, list):
                expanded.extend((item, f"{path}/{index}") for index, item in enumerate(value))
            elif isinstance(value, dict) and token in value:
                expanded.append((value[token], f"{path}/{pointer_token(token)}"))
        matches = expanded
    return matches


def assertion_leaves(value: object, path: str) -> list[tuple[object, object, str]]:
    """Every mutable leaf beneath an assertion block, in document order."""
    leaves: list[tuple[object, object, str]] = []
    if isinstance(value, dict):
        for key, child in value.items():
            if key == "note":
                continue
            child_path = f"{path}/{pointer_token(key)}"
            if isinstance(child, (dict, list)):
                leaves.extend(assertion_leaves(child, child_path))
            elif mutate_leaf(child) is not None:
                leaves.append((value, key, child_path))
    elif isinstance(value, list):
        for index, child in enumerate(value):
            child_path = f"{path}/{index}"
            if isinstance(child, (dict, list)):
                leaves.extend(assertion_leaves(child, child_path))
            elif mutate_leaf(child) is not None:
                leaves.append((value, index, child_path))
    return leaves


def mutate_leaf(value: object) -> object | None:
    if isinstance(value, bool):
        return not value
    if isinstance(value, int):
        return value + 1_000_003
    if isinstance(value, float):
        return value + 1_000_003.0
    if isinstance(value, str):
        return value + "__negative_control__"
    if value is None:
        return "__negative_control__"
    return None


def reject_mutated_assertion(
    *,
    root: Path,
    spec_root: Path,
    build: Path,
    fixture_path: str,
    route: str,
    ordinal: int,
) -> dict[str, object]:
    pristine = json.loads((spec_root / "conformance" / fixture_path).read_text(encoding="utf-8"))
    matches = pointer_matches(pristine, route)
    if not matches:
        raise RuntimeError(f"{fixture_path}#{route}: mutation route did not resolve")
    leaf_count = sum(len(assertion_leaves(value, path)) for value, path in matches)
    if leaf_count == 0:
        raise RuntimeError(f"{fixture_path}#{route}: no mutable assertion leaf")
    not_discriminating: list[dict[str, object]] = []
    with tempfile.TemporaryDirectory(prefix="core-mutant-", dir=build) as temporary:
        mutation_root = Path(temporary) / "conformance"
        shutil.copytree(spec_root / "conformance", mutation_root)
        fixture_file = mutation_root / fixture_path
        for attempt in range(min(leaf_count, MAX_MUTANTS_PER_ROUTE)):
            # A fresh copy per attempt: exactly one leaf differs from the corpus.
            document = json.loads(json.dumps(pristine))
            leaves = [
                leaf
                for value, path in pointer_matches(document, route)
                for leaf in assertion_leaves(value, path)
            ]
            container, key, leaf_path = leaves[attempt]
            before = container[key]
            after = mutate_leaf(before)
            container[key] = after
            detail = {"json_pointer": leaf_path, "before": before, "after": after}
            fixture_file.write_text(json.dumps(document, indent=2) + "\n", encoding="utf-8")
            mutant_manifest = build / f"core-mutant-{ordinal}-{attempt}.txt"
            mutant_manifest.unlink(missing_ok=True)
            mutant_env = os.environ.copy()
            mutant_env.update({
                "LAZILY_SPEC_CONFORMANCE_DIR": str(mutation_root),
                "LAZILY_CONFORMANCE_MANIFEST": str(mutant_manifest),
                "LAZILY_CONFORMANCE_RUN_ID": f"core-mutant-{os.getpid()}-{ordinal}-{attempt}",
            })
            rejection = run_negative_control(
                [str(build / "tests/test_reactive_graph_conformance")],
                env=mutant_env,
                fixture_path=fixture_path,
            )
            if rejection["rejected_by"] == "divergence":
                return {"mutation": detail, **rejection, "non_discriminating_mutants": not_discriminating}
            not_discriminating.append({"mutation": detail, **rejection})
    raise RuntimeError(
        f"{fixture_path}#{route}: no mutated assertion leaf was rejected by a library divergence "
        f"({len(not_discriminating)} mutant(s) failed for another reason: "
        f"{[item['mutation']['json_pointer'] for item in not_discriminating]})"
    )


def parse_manifest(path: Path) -> tuple[set[str], set[tuple[str, str]], dict[str, list[tuple[str, str]]], set[str]]:
    opened: set[str] = set()
    replayed: set[tuple[str, str]] = set()
    declared: dict[str, list[tuple[str, str]]] = {}
    bound: set[str] = set()
    for raw in path.read_text(encoding="utf-8").splitlines():
        fields = raw.split("\t")
        if len(fields) == 1 and fields[0] and not fields[0].startswith("#"):
            opened.add(fields[0])
        elif len(fields) == 3 and fields[0] == "@replayed":
            replayed.add((fields[1], fields[2]))
        elif len(fields) == 3 and fields[0] == "@block_declared":
            fixture, site = fields[1].split("|", 1)
            declared.setdefault(fixture, []).append((site, fields[2]))
        elif len(fields) == 2 and fields[0] == "@block_bound":
            bound.add(fields[1])
    return opened, replayed, declared, bound


def main() -> None:
    root = Path(__file__).resolve().parents[1]
    profile_path = require_env("LAZILY_CORE_PROFILE_PATH")
    spec_root = require_env("LAZILY_CORE_SPEC_ROOT")
    reports_root = require_env("LAZILY_CORE_REPORTS_ROOT")
    report_path = require_env("LAZILY_CORE_REPORT_PATH")
    profile = json.loads(profile_path.read_text(encoding="utf-8"))
    declaration = json.loads((root / "scripts/core-conformance.json").read_text(encoding="utf-8"))
    binding = next(item for item in profile["bindings"] if item["id"] == BINDING_ID)
    required = {item["path"]: item for item in profile["required_fixtures"]}
    routes_document = json.loads((spec_root / "schemas/assertion-blocks.json").read_text(encoding="utf-8"))
    routes: dict[str, list[str]] = {path: [] for path in binding["required_fixtures"]}
    for route in routes_document["x-fixture-routes"]:
        if route["fixture"] in routes:
            routes[route["fixture"]].append(route["pointer"])

    build = root / "build/core-conformance"
    manifest = build / "core-manifest.txt"
    build.mkdir(parents=True, exist_ok=True)
    manifest.unlink(missing_ok=True)
    run([
        "cmake", "-S", str(root), "-B", str(build),
        "-DLAZILY_BUILD_TESTS=ON", "-DLAZILY_BUILD_FFI=ON", "-DCMAKE_BUILD_TYPE=Release",
    ])
    run(["cmake", "--build", str(build), "--target", "test_reactive_graph_conformance", "--parallel"])
    test_env = os.environ.copy()
    test_env.update({
        "LAZILY_SPEC_CONFORMANCE_DIR": str(spec_root / "conformance"),
        "LAZILY_CONFORMANCE_MANIFEST": str(manifest),
        "LAZILY_CONFORMANCE_RUN_ID": f"core-report-{os.getpid()}",
    })
    run([str(build / "tests/test_reactive_graph_conformance")], env=test_env)
    if not manifest.is_file() or manifest.stat().st_size == 0:
        raise RuntimeError("reactive-graph runner produced no runtime manifest")

    opened, replayed, declared, bound = parse_manifest(manifest)
    evidence: dict[str, object] = {
        "schema_version": 1,
        "binding_id": BINDING_ID,
        "producer": "tests/test_reactive_graph_conformance.cpp",
        "execution_flavors": ["single_threaded"],
        "fixtures": {},
    }
    observed_fixtures: list[dict[str, object]] = []
    mutation_ordinal = 0
    for fixture_path in binding["required_fixtures"]:
        definition = required[fixture_path]
        if fixture_path == SKIP_FIXTURE:
            observed_fixtures.append({
                "path": fixture_path,
                "scenario_ids": [],
                "proof_classes": definition["proof_classes"],
                "assertion_blocks": [],
                "status": "skipped",
            })
            continue
        if fixture_path not in opened:
            raise RuntimeError(f"required fixture was not opened: {fixture_path}")
        executed_scenarios = sorted(
            scenario for fixture, scenario in replayed if fixture == fixture_path
        )
        if set(executed_scenarios) != set(definition["scenario_ids"]):
            raise RuntimeError(
                f"{fixture_path}: executed scenarios {executed_scenarios!r} do not match profile "
                f"{definition['scenario_ids']!r}"
            )
        blocks: list[dict[str, object]] = []
        fixture_evidence: dict[str, object] = {"routes": {}}
        for route in routes[fixture_path]:
            sites = [(site, digest) for site, digest in declared.get(fixture_path, []) if route_pattern(route).match(site)]
            if not sites:
                raise RuntimeError(f"{fixture_path}#{route}: runtime manifest declared no matching assertion sites")
            missing = sorted(site for site, digest in sites if digest not in bound)
            if missing:
                raise RuntimeError(f"{fixture_path}#{route}: assertion sites were not consumed: {missing}")
            fixture_evidence["routes"][route] = {
                "status": "mutation-killed",
                "consumed_sites": sorted(site for site, _ in sites),
                "bound_digests": sorted({digest for _, digest in sites}),
                "negative_control": reject_mutated_assertion(
                    root=root,
                    spec_root=spec_root,
                    build=build,
                    fixture_path=fixture_path,
                    route=route,
                    ordinal=mutation_ordinal,
                ),
            }
            mutation_ordinal += 1
            reference = (
                f"{BINDING_ID}/assertion-evidence.json#/fixtures/{pointer_token(fixture_path)}"
                f"/routes/{pointer_token(route)}"
            )
            blocks.append({
                "id": route,
                "consumed": True,
                "discriminability_evidence": reference,
            })
        evidence["fixtures"][fixture_path] = fixture_evidence
        observed_fixtures.append({
            "path": fixture_path,
            "scenario_ids": executed_scenarios,
            "proof_classes": definition["proof_classes"],
            "assertion_blocks": blocks,
            "status": "partial",
        })

    evidence_dir = reports_root / BINDING_ID
    evidence_dir.mkdir(parents=True, exist_ok=True)
    (evidence_dir / "assertion-evidence.json").write_text(
        json.dumps(evidence, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    report = {
        "schema_version": 1,
        "profile_id": profile["profile_id"],
        "binding_id": BINDING_ID,
        "fixture_corpus_digest": profile["fixture_corpus"]["digest"],
        "fixtures": observed_fixtures,
        "runtime_skips": declaration["runtime_skips"],
        "fixtureless_claims": [{
            "id": "c_abi_ffi_boundary",
            "proof_classes": ["native_artifact_consumer"],
            "passed": True,
            "evidence": f"{BINDING_ID}/c-abi-ffi-boundary.json",
        }],
    }
    report_path.parent.mkdir(parents=True, exist_ok=True)
    report_path.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def self_test() -> None:
    """Prove a negative control is credited only for a library divergence.

    Runs without the C++ build: the reactive-graph runner is replaced by a fake
    whose output reproduces the runner's real lines, measured against
    build/core-conformance/tests/test_reactive_graph_conformance on 2026-10-15.
    """
    require_abort = (
        "FAIL: observationally_equal names a scenario that was not replayed "
        "@tests/test_reactive_graph_conformance.cpp:1124\n"
    )
    finding = FINDING_SUFFIX + " (fixture is canonical and is not edited)\n"

    def step_divergence(fixture: str) -> str:
        return f"FAIL: {fixture} step 1 dependents_of.topic: observed 8, expected 1000011{finding}"

    classifier_cases = [
        (require_abort, "other_failure"),
        ("FAIL: fixture asserts readable.x on a node never created @x.cpp:390\n", "other_failure"),
        ("FAIL: reactive-graph fixture set drifted; every fixture must be replayed\n", "other_failure"),
        ("FAIL: reactive-graph conformance replayed nothing (0 fixtures, 0 scenarios)\n", "other_failure"),
        ("Segmentation fault\n", "other_failure"),
        (step_divergence("other_fixture.json"), "other_failure"),
        (step_divergence("target.json"), "divergence"),
        (f"FAIL: target.json/s1 step 0 cleanup_order: observed [a, b], expected [b]{finding}", "divergence"),
        (f"FAIL: target.json scenarios 's1' and 's2' are not observationally equal{finding}", "divergence"),
    ]
    for output, want in classifier_cases:
        got = rejected_by(output, "reactive-graph/target.json")
        if got != want:
            raise AssertionError(f"rejected_by({output.strip()!r}) = {got!r}, want {want!r}")

    real_run = subprocess.run

    def fake_runner(argv, *, env, **_kwargs):
        # Mirrors the C++ runner: a renamed scenario LABEL aborts on the
        # runner's own lookup; a mutated library observation reports a FINDING.
        fixture = Path(env["LAZILY_SPEC_CONFORMANCE_DIR"]) / "reactive-graph/target.json"
        document = json.loads(fixture.read_text(encoding="utf-8"))
        labels = document["expected"].get("observationally_equal", [])
        if any(label.endswith("__negative_control__") for label in labels):
            output = require_abort
        else:
            output = step_divergence("target.json")
        return subprocess.CompletedProcess(argv, -6, stdout=output)

    def drive(expected: dict[str, object]) -> dict[str, object]:
        with tempfile.TemporaryDirectory(prefix="core-report-self-test-") as temporary:
            spec = Path(temporary) / "spec"
            fixture = spec / "conformance/reactive-graph/target.json"
            fixture.parent.mkdir(parents=True)
            fixture.write_text(json.dumps({"expected": expected}), encoding="utf-8")
            build = Path(temporary) / "build"
            build.mkdir()
            return reject_mutated_assertion(
                root=Path(temporary), spec_root=spec, build=build,
                fixture_path="reactive-graph/target.json", route="/expected", ordinal=0,
            )

    subprocess.run = fake_runner
    try:
        # The first two leaves are scenario labels: a runner-bookkeeping abort
        # must be recorded, not credited, and the next leaf tried.
        result = drive({"observationally_equal": ["s1", "s2"], "final": {"x": 1}})
        if result["rejected_by"] != "divergence" or result["mutation"]["json_pointer"] != "/expected/final/x":
            raise AssertionError(f"label mutants were credited or skipped wrongly: {result['mutation']}")
        skipped = [item["mutation"]["json_pointer"] for item in result["non_discriminating_mutants"]]
        if skipped != ["/expected/observationally_equal/0", "/expected/observationally_equal/1"]:
            raise AssertionError(f"non-discriminating mutants not recorded: {skipped}")
        # A route whose every mutant fails for another reason has no kill.
        for labels in (["s1", "s2"], [f"s{index}" for index in range(12)]):
            try:
                drive({"observationally_equal": labels})
            except RuntimeError as error:
                if "no mutated assertion leaf was rejected by a library divergence" not in str(error):
                    raise
                attempts = str(error).count("/expected/observationally_equal/")
                if attempts != min(len(labels), MAX_MUTANTS_PER_ROUTE):
                    raise AssertionError(f"tried {attempts} mutant(s) for {len(labels)} label(s)") from error
            else:
                raise AssertionError("a route with no divergence kill was credited")
    finally:
        subprocess.run = real_run
    print(f"core report self-test OK ({len(classifier_cases)} classifier cases, 3 routes)")


if __name__ == "__main__":
    try:
        if sys.argv[1:] == ["--self-test"]:
            self_test()
            raise SystemExit(0)
        main()
    except Exception as error:
        print(f"core report FAILED: {error}", file=sys.stderr)
        raise SystemExit(1)
