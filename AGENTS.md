# lazily-cpp

C++ port of the lazily reactive family, built on the **Cell kernel**
(`#lzcellkernel`): two concrete handles — `Source<T, M>` (written from outside,
folds under merge policy `M`) and `Computed<T>` (computed from upstream, guarded
by default) — plus value-less `Effect` sinks outside the hierarchy. `Cell` is a
*concept* (a value-bearing node), not a type. Ships the full lazily-spec wire
protocol, CRDT collection types, the lossless tree CRDT, and the command/RPC
message plane.

## Reactive kernel (`#lzcellkernel`, v2)

The public reactive surface is `include/lazily/cell.hpp`:

- **Two concrete handles.** `Source<T, M = KeepLatest>` and `Computed<T>` — no
  `Cell<T, K>` genus, no `Source<M>`/`Formula` kind markers. `M` is `Source`'s
  own policy param; `Source ≡ Source<KeepLatest>`.
- **Constructors on `Context`.** `source(v)` (keep-latest input),
  `source_with<M>(v)` (folding input, formerly `merge_cell<M>`), `computed(f)`
  (guarded; renamed from `formula`, folds in the former `memo`). Reads: `get`.
  Writes: `set`/`merge`.
- **Guarded by default, always (§9.3, DECIDED 2026-07-21).** Every cell is
  guarded — no unguarded mode. `Source` suppresses an equal write; `Computed`
  suppresses an equal recompute (TC39 `Signal.Computed`). The `memo` constructor
  is removed (folded into `computed`).
- **Write protection is a compile error, not a runtime gate (§3/§4).** `set` and
  `merge` exist ONLY on `Source<T, M>`, so `computed.set(...)` fails to compile
  ("no member named 'set'"). Locked by `has_set<>` `static_assert`s in
  `tests/test_cell_kernel.cpp` and the WILL_FAIL build
  `tests/compile_fail_formula_set.cpp`.
- **Eager = an eager `Computed`, not a `Signal` kind (§9.3).**
  `computed(f).eager()` attaches a puller `Effect`; eagerness is an `eager` bit
  on the node plus an `eager_by_` side table in `Context`, cleared on
  `.lazy()`/dispose (`is_eager()` queries it). Because the puller is an ordinary
  scheduled effect, batched invalidations coalesce into one recompute — the
  `#lzsignaleager` per-write-puller defect is structurally unwritable.
- **`Slot` is the STORAGE sense only (§5.0).** `SlotId`, `SlotNode`, the arena
  free-list, and the wire `SlotValue` are unchanged — a slot is the position that
  holds a node of any kind. Only the reactive-VALUE sense of "slot" became
  `Computed`.

Engine substrate (kept, not the public vocabulary): the internal handle types
`CellHandle<T>` / `SlotHandle<T>` / `Effect`, and the engine constructors
`cell`/`slot` (unguarded) / `memo` (guarded) / `signal`, plus the
`MergeCell<T, Policy>` class in `merge.hpp`, remain as the lower-level surface the
CRDT/relay/coordination families build on. `AsyncContext` remains a stub with no
dependency graph and was not migrated (it carries the vocab only).

## Commit & Push

Commit and push completed work at the end of every turn that changed code,
tests, docs, or fixtures — do not leave finished work uncommitted. Run `make
check` first and ensure it is green; stage only the files that belong to the
change (never secrets or private customer names — see the workspace
`runbooks/private-name-hygiene.md`); write a concise commit message in the
repo's existing style; push to the current branch on `origin`. This standing
rule overrides the harness default of "commit only when explicitly asked" for
this repo.

<!-- tsift:code-navigation v=0.1.77 -->
## Code Navigation

Keep this block self-contained for Codex/OpenCode prompt reuse. If this repository also ships current `.claude/skills/tsift/SKILL.md` or `runbooks/code-navigation.md`, use those deeper runbooks for command detail instead of expanding this block.

Run `tsift status` at session start from the owning repo root. If the task or file lives under a git submodule (for example `src/tsift/...`), switch to that submodule root first so the harness loads the narrower local instructions and repo state instead of the superproject root. If status prints a `run:` recommendation for stale or missing tsift state, run `tsift status --fix` before relying on tsift results; when the harness cannot perform write commands, ask the user to run the printed command instead. Codex projects can install a prompt-time auto-reindex hook with `tsift init --codex`; OpenCode projects can install per-project tsift command shortcuts with `tsift init --opencode`.

Use the commands listed in its `use:` output:
- `tsift --envelope source-read <file> --budget normal` — AST-symbol projection with span metadata and source-window expansion commands (prefer over cat/head for source code files)
- `tsift --envelope symbol-read <symbol> --budget normal` — token-budgeted symbol body, AST span metadata, child refs, and graph/source expansion commands
- `tsift --envelope search <query> --budget normal` — AST-aware hybrid search preview (prefer over grep/rg)
- `tsift --envelope explain <symbol> --budget normal` — callers, callees, community preview
- `tsift graph <symbol> --callers` / `--callees` — call graph navigation
- `tsift summarize <symbol>` — cached summary (only when listed in `use:`)
- `tsift workflow search` — ordered exact/search/explain/summarize/digest recipe that preserves result handles across expansions

When a search envelope includes `report.scale_guard`, run one of its `narrow_commands` before dispatching parallel agents. The guard means the original result set or corpus is broad enough that fan-out should start from a narrower cited handle, path, or exact query.

Prefer bounded digest commands over raw transcript, diff, and verbose-log reads:
- `tsift --envelope session-review <path> --next-context --budget normal` or `tsift --envelope context-pack <path> --budget normal` instead of replaying long session docs, JSONL transcripts, or agent-doc runtime logs with `cat`, `tail`, or `sed`.
- `tsift diff-digest [path]` (`--cached`, `--revision <rev>`) instead of `git diff`, `git show`, or patch-style `git log`.
- `tsift --envelope digest-runner --kind test --path . --shell-command '<test command>'` / `tsift --envelope digest-runner --kind log --path . --shell-command '<build command>'` for noisy test/build/install output, or let the rewrite/hooks create those artifact-backed envelopes for `cargo test`, `pytest`, and verbose cargo commands.
- If RTK is installed, digest-runner delegates supported generic command families through `rtk rewrite` and records the chosen compact filter in `report.filter` while preserving tsift artifact handles.
- Codex, OpenCode, and other harnesses without Claude-style `PreToolUse` hooks should run `tsift rewrite --run '<command>'` before broad `rg`/recursive grep, raw transcript/session/log reads, `git diff`/`git show`/single-patch `git log`, `cargo test`/`pytest`, and cargo build/check/clippy/install commands so the same search, session-digest, diff-digest, and digest-runner rewrites apply manually. OpenCode can install this path as `/tsift-rewrite-run` with `tsift init --opencode`.

For local verification, run `make check` before committing. After local changes, check the latest GitHub Actions CI run with `gh run list --workflow CI --limit 1` and fix any failing tests before calling the work complete.

Only read full source files when tsift results are insufficient.
<!-- /tsift:code-navigation -->
