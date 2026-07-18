# Tool-output condensation (deterministic command-aware)

Tool output (test-runner logs, compiler/linter dumps, build progress) is the largest and
most signal-sparse contributor to a coding agent's context. Most of that volume is not
signal: progress bars, passing-test transcripts, boilerplate, repeated lines.

**Tool-output condensation** is a context-economizer lever that condenses **recognized**
command output at the tool-execution seam, using **deterministic, command-aware rules** (no
LLM, so it is ~free). It knows that a test runner's value is its *failures*, a compiler's is
its *diagnostics*, and drops the rest, **losslessly**: the full raw output is spilled to
disk and a recovery pointer is left in the condensed result, so nothing is destroyed.

It is part of the economizer's **safe** tier (and therefore **on by default**):
lossless-on-demand, fail-open, no-over-reduction. It complements (does not replace) the
aggressive-tier size-based body compression, which is the fallback for unrecognized output
when that tier is active.

## Configuration

Condensation has **no independent toggle** — it is one lever of the single
[`economizer`](economizer.md) tier:

```yaml
economizer: safe        # off | safe | aggressive   (default: safe)
```

Condensation runs on **`safe`** and **`aggressive`**; **`economizer: off`** disables it (along
with all other reduction — verbatim passthrough). See [SETTINGS.md](../SETTINGS.md) and
[the economizer overview](economizer.md).

## What it does

Applied at the **delegate bash-tool seam** ([`tool_bash`](../../src/posix/agent_tools.c)),
before the size-based compression:

1. **Recognize** the command. Compound/piped/substituted lines and unknown commands pass
   through unchanged (fail-open). Common wrappers are unwrapped (`env`, `sudo`, `time`,
   `npx`, `uv run`, `poetry run`, `pnpm exec`, …). `xargs`, `make`, shell scripts, and **any
   path-prefixed invocation** (`./git`, `/tmp/cargo`) are treated as opaque. A family rule
   only ever applies to a bare command name.
2. **Condense** by family:
   - **Test runners** (`pytest`/`jest`/`vitest`/`ctest`/`cargo|go|npm|… test`): keep the
     summary + **every failure verbatim**, drop passing-case transcripts.
   - **Compilers / linters** (`tsc`/`eslint`/`ruff`/`mypy`/`gcc`/`clang`/`rustc`/`cmake`,
     and `cargo|go|dotnet|npm|… build/check/clippy/vet/lint`): keep every error/warning/note
     and diagnostic, drop the `Compiling…/Downloading…/Checking…` progress.
3. **Spill + point.** The full raw output is **atomically** written to
   `<aimee_home>/tool-spills/<ref>.out` (temp → `fsync` → rename → dir `fsync`, mode `0600`,
   so a partial write is never promoted) and the condensed result ends with a pointer
   carrying the opaque `ref`. The agent retrieves the full original with the dedicated
   **`tool_output_get`** tool (P2): one recovery handle, not a raw filesystem path. The
   spill store is kept under a 64 MB budget by oldest-first (mtime) eviction.

## Safety contract

- `economizer: off` ⇒ tool output is **byte-identical** to today (verbatim passthrough).
- **Lossless-on-demand.** A condensed body is only ever produced when the full raw output
  was **durably spilled** first; a failed spill, no spill dir, or a would-be-truncated
  recovery pointer all fall through to passthrough. Nothing is dropped without a backstop.
- **Never hide a failure.** A test runner or build with a **non-zero exit and no recognized
  failure line** passes through verbatim rather than risk eliding the cause.
- **Fail-open everywhere.** An unrecognized command, an over-ceiling body (> 2 MB), or any
  filter error degrades to passthrough (or, on the aggressive tier, the size-based body
  compression), never a crash or a dropped
  result.
- **No masquerade.** A path-prefixed command whose basename matches a known tool (`./git`)
  never inherits that tool's family filter.
- **Not a redaction layer.** Condensation does not *widen* exposure beyond what the raw
  tool result already showed the model, but it does not scrub secrets from the output or the
  spill. Spill files are per-user (`0700` directory) and excluded from log/trace exports by
  default.

## Observability + recovery cost (P4)

Process-wide counters accumulate realized savings **and recovery cost** on live traffic
([`tool_condense_stats_snapshot`](../../src/modules/economizer/tool_condense.c)): `recognized`, `applied`,
`applied_raw` vs `applied_final` bytes, per-family (`test`, `diag`) counts, and the
**recovery-cost** side: `recovered` (successful `tool_output_get` page-backs) and
`recovered_bytes` (bytes re-injected). Two derived fields make the promotion-gate metric
explicit:

- `saved_bytes` = `applied_raw − applied_final` (gross condensation saving);
- **`net_saved_bytes`** = `saved_bytes − recovered_bytes` (**net of recovery**).

`net_saved_bytes` is the net after recovery: if the agent keeps paging spilled output back in,
`recovered_bytes` rises toward `saved_bytes` and the net collapses, the signal that the
lever is not net-saving on that workload. Each condensation logs its `raw→final` delta and
each recall logs `tool_output_get recovered N bytes` under the `tool_condense` module, so the
two sides are greppable together. (This precise per-call channel exists because
`tool_output_get` is the single dedicated recovery handle from P2; `history_fold`/`compress`
recovery via fold-recall remains best-effort, not byte-exact.)

## Scope & rollout

Shipped as the **delegate surface**, **default-ON** (the safe-tier lever): when on, the
delegate bash seam captures the full output (up to a 2 MB ceiling) so the lever sees all of
it, condenses recognized families losslessly, and spills the raw for recovery. Set
`economizer: off` to disable it (along with all other reduction).

The **default-ON** flip landed in unified-economizer **P1c**, justified by the deterministic
gate (lossless-on-demand + fail-open + a no-over-reduction audit); it replaces the old lossy
32 KB read-cap truncation with lossless-recoverable condensation.

The dedicated **`tool_output_get`** retrieval tool + the atomic-write / bounded-eviction
recovery contract landed in unified-economizer **P2**.

**Carried follow-ups** (not yet shipped): the **primary-agent** surface (a client-side
hook); additional command families (VCS `git`, file/dir ops, package managers); and folding
the fold-recall / `code_span_get` recovery flows through the same `tool_output_get` handle.
