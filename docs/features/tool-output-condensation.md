# Tool-output condensation (deterministic command-aware)

Tool output — test-runner logs, compiler/linter dumps, build progress — is the largest and
most signal-sparse contributor to a coding agent's context. Most of that volume is not
signal: progress bars, passing-test transcripts, boilerplate, repeated lines.

**Tool-output condensation** is a context-economizer lever that condenses **recognized**
command output at the tool-execution seam, using **deterministic, command-aware rules** (no
LLM, so it is ~free). It knows that a test runner's value is its *failures*, a compiler's is
its *diagnostics*, and drops the rest — **losslessly**: the full raw output is spilled to
disk and a recovery pointer is left in the condensed result, so nothing is destroyed.

It is **default-ON** (unified-economizer P1c — a safe-tier lever: lossless-on-demand,
fail-open, no-over-reduction) and complements — does not replace — the size-based
`reduce.compress`, which stays the fallback for unrecognized output. Set
`reduce.command_filter=false` to opt out.

## Configuration

```yaml
reduce:
  command_filter: true    # DEFAULT ON. Set false to opt out.
```

Turn it on from the web UI (**⚙️ Settings → `reduce.command_filter`**, see
[SETTINGS.md](../SETTINGS.md)) or the config above. When off, tool output is **byte-identical to today** — the seam falls through to the size-based `reduce.compress`.

## What it does

Applied at the **delegate bash-tool seam** ([`tool_bash`](../../src/posix/agent_tools.c)),
before the size-based compression:

1. **Recognize** the command. Compound/piped/substituted lines and unknown commands pass
   through unchanged (fail-open). Common wrappers are unwrapped (`env`, `sudo`, `time`,
   `npx`, `uv run`, `poetry run`, `pnpm exec`, …). `xargs`, `make`, shell scripts, and **any
   path-prefixed invocation** (`./git`, `/tmp/cargo`) are treated as opaque — a family rule
   only ever applies to a bare command name.
2. **Condense** by family:
   - **Test runners** (`pytest`/`jest`/`vitest`/`ctest`/`cargo|go|npm|… test`): keep the
     summary + **every failure verbatim**, drop passing-case transcripts.
   - **Compilers / linters** (`tsc`/`eslint`/`ruff`/`mypy`/`gcc`/`clang`/`rustc`/`cmake`,
     and `cargo|go|dotnet|npm|… build/check/clippy/vet/lint`): keep every error/warning/note
     and diagnostic, drop the `Compiling…/Downloading…/Checking…` progress.
3. **Spill + point.** The full raw output is written to `<aimee_home>/tool-spills/<ref>.out`
   (mode `0600`) and the condensed result ends with a pointer to that path, which the
   delegate reads back with its own bash tool if it needs an elided passing case.

## Safety contract

- `reduce.command_filter: false` ⇒ tool output is **byte-identical** to today.
- **Lossless-on-demand.** A condensed body is only ever produced when the full raw output
  was **durably spilled** first; a failed spill, no spill dir, or a would-be-truncated
  recovery pointer all fall through to passthrough. Nothing is dropped without a backstop.
- **Never hide a failure.** A test runner or build with a **non-zero exit and no recognized
  failure line** passes through verbatim rather than risk eliding the cause.
- **Fail-open everywhere.** An unrecognized command, an over-ceiling body (> 2 MB), or any
  filter error degrades to the size-based `reduce.compress` — never a crash or a dropped
  result.
- **No masquerade.** A path-prefixed command whose basename matches a known tool (`./git`)
  never inherits that tool's family filter.
- **Not a redaction layer.** Condensation does not *widen* exposure beyond what the raw
  tool result already showed the model, but it does not scrub secrets from the output or the
  spill. Spill files are per-user (`0700` directory) and excluded from log/trace exports by
  default.

## Observability

Process-wide counters accumulate realized savings on live traffic
([`tool_condense_stats_snapshot`](../../src/tool_condense.c)): `recognized`, `applied`,
`applied_raw` vs `applied_final` bytes, and per-family (`test`, `diag`) counts. Each
condensation also logs its `raw→final` delta under the `tool_condense` module.

## Scope & rollout

Shipped as the **delegate surface**, **default-ON** (the safe-tier lever): when on, the
delegate bash seam captures the full output (up to a 2 MB ceiling) so the lever sees all of
it, condenses recognized families losslessly, and spills the raw for recovery. Opt out with
`reduce.command_filter=false`; roll back the default by the same flag.

The **default-ON** flip landed in unified-economizer **P1c**, justified by the deterministic
gate (lossless-on-demand + fail-open + a no-over-reduction audit); it replaces the old lossy
32 KB read-cap truncation with lossless-recoverable condensation.

**Carried follow-ups** (not yet shipped): the **primary-agent** surface (a client-side hook
+ a first-class `tool_output_get` retrieval tool); additional command families (VCS `git`,
file/dir ops, package managers); and unifying the spill with the other economizer recovery
handles (unified-economizer P2).
