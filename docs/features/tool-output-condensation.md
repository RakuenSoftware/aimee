# Tool-output condensation (deterministic command-aware)

Tool output — test-runner logs, compiler/linter dumps, build progress — is the largest and
most signal-sparse contributor to a coding agent's context. Most of that volume is not
signal: progress bars, passing-test transcripts, boilerplate, repeated lines.

**Tool-output condensation** is a context-economizer lever that condenses **recognized**
command output at the tool-execution seam, using **deterministic, command-aware rules** (no
LLM, so it is ~free). It knows that a test runner's value is its *failures*, a compiler's is
its *diagnostics*, and drops the rest — **losslessly**: the full raw output is spilled to
disk and a recovery pointer is left in the condensed result, so nothing is destroyed.

It is **default-OFF** and complements — does not replace — the size-based
`reduce.compress`, which stays the fallback for unrecognized output.

## Configuration

```yaml
reduce:
  command_filter: false   # DEFAULT OFF. The whole lever.
```

Turn it on from the web UI (**⚙️ Settings → `reduce.command_filter`**, see
[SETTINGS.md](../SETTINGS.md)) or the config above. When off, tool output is **byte-identical
to today** — the seam falls through to the size-based `reduce.compress`.

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
- **Fail-open everywhere.** An unrecognized command, an over-cap body (> 1 MiB), or any
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

Shipped as the **delegate surface** (default-off). Enable it on a validation host, watch the
savings counters, and roll back by flipping `reduce.command_filter` to `false`.

**Carried follow-ups** (not yet shipped): the **primary-agent** surface (a client-side hook
+ a first-class `tool_output_get` retrieval tool); additional command families (VCS `git`,
file/dir ops, package managers); and the **operator default-ON** decision — a named gate
(no-lost-signal audit + material realized savings + fail-safe, measured via the counters
above), never a silent flip.
