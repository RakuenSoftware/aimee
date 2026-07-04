# Proposal: Deterministic command-aware tool-output condensation

- **State:** proposed — design (pending review). A new **deterministic (non-LLM),
  command-aware** condensation lever for the unified context economizer that shrinks
  **tool/command output** at the execution seam, before it enters agent context.
  Default-off, lossless-on-demand, config-gated, integrated with the existing `reduce_*`
  subsystem and its telemetry ledger. Design roundtable-reviewed (1 round, no blocking
  items); the refinements below (spill retrieval + lifetime, wrapper-unwrapping slice,
  filter isolation, ledger schema, per-family failure policy, spill security,
  delegate-first thesis) are incorporated.
- **Thesis:** the largest and most signal-sparse contributor to context growth in a
  coding agent is **tool output** — test-runner logs, compiler/linter dumps, VCS status,
  directory listings, package-manager chatter. Most of that volume is not signal:
  progress bars, passing-test transcripts, boilerplate banners, and repeated log lines.
  The existing economizer levers reduce this **generically** (size-based body
  compression, history folding) — they do not *understand the command*, so they either
  truncate blindly (losing the one failing line at the bottom of a 400-line test log) or
  leave the noise in. A **command-aware** filter that knows "for a test runner, keep the
  failures and drop the passes" removes the bulk of the noise **losslessly** (the full
  output is spilled and one retrieval away), at **zero model cost** (pure deterministic
  transforms, no summarization call). Scope is **delegate-side first** (aimee runs the
  tool → aimee filters the output); the primary-agent hook is a later, independently-gated
  surface, so the delegate lever can reach default-on without waiting on the hook.

## Charter roles
Extends **Reduce** (the context economizer: `context_reduce` / `context_fold` /
`compact_body`). This is a **new lever**, not a new subsystem: a command-aware filter
at the **tool-execution seam** (where a command's stdout/stderr becomes a tool-result),
complementing the existing size-based `reduce_compress` (which stays the fallback for
unrecognized output) and the `/v1` gateway/delegate seams. No change to any existing
reduction algorithm; no LLM in the path.

## Goal
Cut tool-output tokens on recognized commands by a large margin **without ever losing
information the agent can't recover**: every condensation is backed by a full-output
spill and a first-class retrieval tool, so the agent re-reads the raw output on demand.
The feature is default-off behind a config flag, filters **only** commands with an
explicit registered rule (everything else passes through untouched), and is measured
against the economizer's existing token-delta ledger before any default-on discussion.

## §0 Why a command-aware lever (vs the existing size-based ones)
The economizer already has two tool-output-adjacent levers:

- `reduce_compress` — **boundary-free, size-based** compression of oversized tool-result
  **bodies**. It caps/compresses by byte size. It cannot tell a passing-test line from a
  failing one, so to stay safe it triggers only on very large bodies and keeps whatever
  fits — the signal (a failure at the tail) can be exactly what falls outside the cap.
- `reduce_history_fold` / gateway mutation — reduce **across turns / at the wire**, not
  the individual command output as it is produced.

The gap: **per-command semantic judgement**. A test runner's value is its *failures*; a
`status` command's value is a *one-line summary*; a linter's value is *errors grouped by
file*; a package install's value is *"ok" or the error*. None of that is expressible as a
byte cap. This lever encodes that judgement as deterministic per-command rules.

## §1 Core concept — a filter registry over the tool seam
A registry maps a **recognized command** (matched on a normalized argv[0] + subcommand,
after wrapper-unwrapping — §2.2) to a **filter function** that transforms
`(exit_code, stdout, stderr)` → `(condensed_text, spill_ref)`.

Design invariants:

1. **Allowlist, not blocklist.** An output is condensed **only** if its (unwrapped)
   command has a registered rule. Anything unrecognized is returned verbatim (fail-open
   — never surprise the agent with a filtered output it didn't opt into).
2. **Lossless-on-demand, with a concrete retrieval affordance.** The full, unfiltered
   `(stdout, stderr)` is written to a spill file; the condensed text carries a trailing
   pointer (`… N lines elided · full output: aimee tool-output <spill_ref>`). Retrieval is
   a **first-class aimee tool** (`tool_output_get(spill_ref[, grep])`) that returns the
   raw bytes (or a grep over them) — it does **not** assume the agent has an arbitrary
   file-read/shell tool for the scratch path. The tool is registered as part of this
   lever so recovery is always available where the lever is enabled. The retrieval tool
   is itself isolation-wrapped: a hard cap on returned bytes (default 1 MiB) and a bounded
   grep, so a broad retrieval can't re-inject the very volume this lever removed.
   **If the spill write fails or its durability can't be confirmed before returning to the
   model, the filter degrades to verbatim passthrough** (ledger `spilled=false`,
   `reducer=none`) — never a condensed body without a recoverable backstop.
3. **Failure-biased, per family.** On a non-zero exit, filters keep *more*, never less;
   any unrecognized non-zero exit is **verbatim passthrough + spill** (never risk hiding
   the cause). Each family's failure policy is stated explicitly (§2.2).
4. **Deterministic, cheap, and isolated.** Pure string/line transforms; no network, no
   model. Each filter runs inside an isolation wrapper: a hard input-size cap (above which
   it declines and hands the body to `reduce_compress`), a wall-clock timeout, a bounded
   output size, and a per-filter circuit breaker — a filter that throws, times out, or
   trips the breaker is **disabled for that command family only** (not the whole feature)
   and its output falls back to raw + spill. Breaker state is **per-run** (not persisted
   across runs unless an operator opts in); it trips after N consecutive failures on a
   family (default 3) and re-enables only after M successful invocations on that family
   (default 5).

## §2 Design

### §2.1 Filter primitives
A small toolbox the per-command rules compose from:

- **Strip** — drop content-free lines: progress bars/spinners, ANSI control noise,
  download/percentage tickers, boilerplate banners, blank runs.
- **Group** — aggregate like items: files by directory, diagnostics by file, warnings by
  category — with counts (`23 files under src/ (list elided)`).
- **Truncate-with-signal** — keep a head + tail window and the *matched signal lines*
  (errors/failures/conflict-markers) in between, not a blind prefix.
- **Dedup** — collapse repeated/near-repeated lines to `<line> (×N)`.
- **Select** — for structured runners, keep only the salient partition (failures,
  changed files, the summary line) and drop the rest.

### §2.2 Command recognition + wrapper unwrapping (S2, before any family)
Recognition normalizes the invocation before registry lookup:

- **Unwrap known single-command wrappers** iteratively: `npx`, `pnpm exec`, `bun x`,
  `uv run`, `poetry run`, `pipenv run`, `time`, `env VAR=…`, `nice`, `sudo -…` → strip to
  the inner command. `pnpm test` / `npm test` / `yarn test` resolve to the runner family
  by the script name when it is a known alias (`test`, `build`, `lint`).
- **Do NOT unwrap multiplexers.** `xargs` (and similar) can run *many* commands whose
  outputs interleave on one stream — unwrapping to a single family would misclassify it.
  Treat such invocations as **opaque** (generic summary+FAIL fallback only, never a
  family rule).
- **Sniff indirection** for `make <target>` and `./script.sh` / `bash script`: these are
  **not** unwrapped to a specific family (their body is arbitrary) — they get the generic
  "summary + FAIL-lines, verbatim-on-non-zero" fallback, never a family-specific rule.
- **Unrecognized ⇒ passthrough.** The ledger records recognition outcome
  (`unrecognized` vs `recognized`), so §4's report distinguishes "we don't know this
  command" from "we know it but it yielded little."

**Command families** (each an independent slice after S2), with an explicit failure
policy:

1. **Test runners** (highest yield) — surface the summary line + **every** failing case
   verbatim (name, assertion, location); drop passing-case transcripts. On non-zero with
   no parseable failures, verbatim passthrough. Recognizers for common runners + a generic
   "summary + FAIL-lines" fallback.
2. **Compilers / linters** — group diagnostics by file, keep error text verbatim, drop
   progress/"compiling N/M"; when errors present, collapse warnings to a count. Non-zero
   with no parseable diagnostics ⇒ verbatim passthrough.
3. **VCS** — `status` → porcelain one-liner; `log` → subject-line list; `diff` → per-file
   add/del summary **but keep conflict markers (`<<<<<<<`), `Binary files … differ`, and
   rename/mode lines verbatim**; `push`/`pull` → the result line; **any non-zero ⇒
   verbatim passthrough**.
4. **File / directory ops** — large listings grouped by directory with counts; `grep`
   results deduped + capped with a match count, paths kept. Non-zero ⇒ passthrough.
5. **Package managers / build tools** — drop resolution/download progress, keep the final
   status line; **non-zero ⇒ keep the first error block verbatim + passthrough the tail**.

### §2.3 Attach surfaces (delegate first)
- **Delegate tool execution (server-side) — the primary surface.** aimee runs the tool
  for its own delegates; the filter applies to captured stdout/stderr **before** it
  becomes a tool-result. Fully server-controlled, no client cooperation. This surface is
  self-contained and reaches its own default-on decision (§6) independently.
- **Primary-agent hook — a later, separately-gated surface.** aimee already manages the
  primary's client-side tool hooks (a `PreToolUse`/`PostToolUse` mechanism is registered
  today, and a command-rewriting hook already ships). A later slice routes a recognized
  primary-agent command through the **same** registry via that mechanism. Its trust/
  contract shape (rewrite-the-command vs filter-the-result; the filtered output must
  preserve the agent's mental model + carry the same retrieval pointer) is settled in that
  slice, and it has its **own** validation gate — the delegate surface does not wait on it.

### §2.4 Integration with the economizer + ledger schema
- New config lever `reduce_command_filter` (default **off**) gates the feature, alongside
  the existing `reduce_*` flags — so it appears in the typed config surface and the web
  Settings page automatically.
- **Ledger schema (explicit, no double-counting):** one **pipeline** row per tool-result
  body — `raw_bytes` (the original), `final_bytes` (after this lever *and* any downstream
  `reduce_compress`), plus `command_family`, `recognized` (bool), `spilled` (bool), and
  `reducer` = the deepest reducer that touched the body (`command_filter` or `compress`).
  Rows are **exclusive** (one per body); savings attributed to the deepest reducer so a
  body reduced by both is never counted twice.
- The size-based `reduce_compress` is the **fallback**: unrecognized commands, over-cap
  bodies, or a circuit-broken family still get today's behavior.

### §2.5 Spill security + lifetime
- **Location + perms:** spill root is **per-run, per-user**, mode `0700`, under the run's
  scratch area — never a shared/world-readable path.
- **Content posture (honest):** spill files contain raw tool output, which may include
  secrets that were already in that output. This lever does **not** claim to redact
  secrets (it is not a redaction layer); it simply **does not widen** exposure beyond what
  the raw tool-result already would have shown the model. Spill content is **excluded by
  default from log/trace/error exports** so condensation does not *newly* persist raw
  output into telemetry.
- **Lifetime (concrete):** a spill is available for **the run plus the immediately
  following user turn**; swept at run end (unless an explicit keep flag). A retrieval
  against an expired ref returns a clear **`spill expired`** marker (not a silent empty),
  so the agent knows recovery is unavailable rather than assuming no output.
- **Growth + concurrency:** a per-run total-spill cap with **oldest-first eviction**.
  `spill_ref` is an **opaque random token** (not a monotonic counter — not enumerable to
  a co-tenant even under `0700`). Eviction is **atomic** (tombstone manifest / rename),
  so a retrieval racing a parallel eviction sees a clean `spill expired` marker, never a
  half-deleted file.

## §3 Configuration
- `reduce_command_filter` (bool, default off) — master gate (delegate surface).
- Start with **one master flag** (roundtable §9); per-family sub-flags only if the
  validation data shows a family needs independent control. Even with the master flag on,
  aggressive-truncation filters start in their safest mode.
- **Input cap** (default **1 MiB** per body) above which the filter declines → hands off
  to `reduce_compress`. Operator-tunable.
- Escape hatch: a `config`/env toggle forcing raw passthrough (debugging a suspected
  over-filter), plus a per-invocation "raw" affordance.

## §4 Telemetry / observability
- The §2.4 ledger rows drive a read-only summary (savings by family over a window).
- Two audits, **percentile-based against the window's filtered population** (operator-
  tunable): suspected **over-filter** (top-1% savings ratio — possible signal loss) and
  suspected **under-recognition** (commands frequently `unrecognized`, or `recognized`
  with bottom-decile yield — possible missing wrapper/recognizer).

## §5 Implementation plan (slices, each roundtable-gated + default-off)
- **S1 — registry + primitives + config + spill/retrieval.** The filter-registry data
  structure, the §2.1 primitives, `reduce_command_filter` config field (→ config_fields[]
  → Settings), the isolation wrapper (§1.4), the spill store + `tool_output_get` retrieval
  tool + lifetime/eviction, unit tests on primitives + spill. No seam wired.
- **S2 — recognition + wrapper unwrapping.** The normalization/unwrap pipeline (§2.2) +
  the ledger `recognized` outcome, with fixtures for the common wrappers. No family rules
  yet (everything recognized-but-unhandled ⇒ passthrough).
- **S3 — delegate seam + test-runner family.** Wire the delegate tool-execution output
  through the registry when the flag is on; ledger rows; `reduce_compress` fallback; the
  test-runner family as the proving case (fixtures for pass, fail, non-parseable).
- **S4 — remaining families.** Compilers/linters, VCS, file ops, package managers — each
  a small reviewed increment with pass/fail fixtures.
- **S5 — observability surface.** The §4 savings + over/under-filter report.
- **S6 — delegate-surface validation + default-on decision.** §6 gates on a real
  workload; default-on decision for the delegate surface only.
- **S7 — primary-agent hook (separately gated).** Route recognized primary-agent commands
  through the same registry via the hook; its own trust model + validation gate.

## §6 Validation gates (per surface, before its default-on)
1. **No lost signal.** On a corpus of failing builds/tests, the **condensed** output
   always contains every failure the raw output did (the spill is a backstop, but the
   condensed text alone must not hide a failure). Per-family test requirement: e.g. test
   runners must emit every `FAIL` line.
2. **Measured savings.** Ledger shows a material token reduction on recognized commands
   without regressing task success on an agent test workload.
3. **Fail-safe proven.** Unrecognized commands / over-cap bodies / a throwing/timing-out
   filter all degrade to raw output + spill (or `reduce_compress`), never to a crash, a
   hang, or a dropped result; the per-filter breaker isolates a bad family.

## §7 Risks
- **Over-filtering hides signal.** Mitigated by per-family failure-bias, the lossless
  spill + retrieval, the allowlist, and the over-filter audit; default-off until §6.
- **Command mis-recognition** (wrapper/alias/unusual invocation) → unwrap-then-passthrough
  on doubt; a family rule never fires on an unconfirmed command.
- **Spill growth / durability** — capped, oldest-first eviction, `spill expired` marker so
  the loss is explicit; §2.5.
- **Filter bug** — isolation wrapper (timeout, size guard, breaker) contains it to one
  family; §1.4.
- **Determinism drift across tool versions** — recognizers match stable structure
  (summary lines, exit codes) and degrade to passthrough when the shape is unfamiliar.

## §8 Non-goals
- No LLM/summarization in the path (that is a different, existing lever).
- No secret **redaction** (this lever does not widen exposure, but does not scrub it).
- No change to what commands the agent may run, or to command *execution* — only to how
  the *output* is presented to the model.
- Not a replacement for `reduce_compress` — it is the command-aware layer *above* it.

## §9 Open items (for roundtable)
1. **Config granularity** — confirm the one-master-flag start vs per-family sub-flags.
2. **Retrieval tool shape** — `tool_output_get(spill_ref)` returns full bytes vs a
   grep/window interface by default (cost of re-injecting a large raw body).
3. **Primary-hook contract (S7)** — rewrite-the-command vs filter-the-result; confirm the
   hook surface can carry the retrieval pointer without breaking the tool contract.
4. **`reduce_compress` hand-off** — final cap value + whether both ever run on one body
   (the ledger models it as a pipeline either way).
