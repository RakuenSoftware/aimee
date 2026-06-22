# Impl plan: four-part harness taxonomy

> **Plan-roundtable status:** converged in two rounds. The automated multi-model
> panel is unavailable in this environment (no built `aimee` binary, no
> forge-brokered delegate endpoints), so the gate ran as a four-lens adversarial
> review panel of subagents — grounding/correctness, scope/altitude, contrarian
> skeptic, and tests/verification. Round 1 returned 2 approve / 2 request_changes
> with two real code bugs surfaced (bare-substring marker matching, and a context
> classifier that missed the successful-refetch case). All blocking and major
> findings were folded (see "Review findings folded" below) and the reference tools
> re-verified green. **Round 2 returned 4 approve / 0 request_changes (unanimous),
> with reviewers running the self-tests.** Its non-blocking findings were also
> folded: a polling false-positive caveat on the byte-identical refetch rule (doc +
> code comment), `unrecognized arguments` (plural) for argparse, high-frequency
> inflected control markers, and two new self-tests (runaway-suppression path,
> argparse plural). A 2nd independent-model review remains available once a healthy
> delegate worker is reachable.
>
> **Round 3 (live multi-model roundtable):** the real panel became reachable over
> the `/v1` UDS, so the code was re-reviewed by the API agents (two focused parallel
> runs, one per tool; 2 of 3 panelists answered each). Findings triaged on merit —
> the deciding ones folded, the misreads rejected with rationale (see "Review
> findings folded — round 3" below). Both tools re-verified green: `classify_failures`
> now 15 self-test assertions, `delete_pressure` 6.

Plan for [`four-part-harness-taxonomy.md`](four-part-harness-taxonomy.md). This is
a **vocabulary + measurement-tooling** proposal, not an intelligence-surface
proposal, so it does not inherit the Architecture Charter review gate. Its concrete,
shippable deliverable is a **canonical vocabulary** plus **two runnable reference
tools** that turn two slogans ("context is our biggest tax", "delete harness code
as models improve") into measured numbers. The in-process C ports are **specced as
explicit follow-ons** and are out of scope for this PR — they require the build
toolchain and a live DB1/DB2 runtime that the reference tools deliberately do not.

## Scope of THIS PR (`docs(proposal)` + reference tooling)

In scope, all verifiable without a build:

1. The proposal document (vocabulary §1, subsystem map §2, classifier §3,
   delete-pressure §4, durable bets §5, rollout §6, verification status §7).
2. [`scripts/harness/classify_failures.py`](../../../scripts/harness/classify_failures.py)
   — the failure classifier, with `--self-test`.
3. [`scripts/harness/delete_pressure.py`](../../../scripts/harness/delete_pressure.py)
   — the delete-pressure metric, with `--self-test`.
4. [`scripts/harness/README.md`](../../../scripts/harness/README.md) — how to run
   both, what they do and do not claim.
5. The `docs/PROPOSALS.md` index entry under **Pending**.

**Explicitly out of scope** (named here so the boundary is a decision, not a
silent gap), each a follow-on once this lands and the proposal is accepted:

- The C port `harness_classify_failure()` in `src/trace_analysis.c` and the
  `aimee trajectory classify [--json]` subcommand — needs build + DB1.
- The `aimee guardrails anti-patterns export --json` emitter — needs build + DB2.
- The scaffold A/B harness (flag-gating each tool prompt, comparing the §3 tool
  failure rate with/without) — needs a runtime and an eval loop.
- Landing the vocabulary into `MANUAL.md` and trace/diagnose output (rollout §6.1).
  Cheap, but it is a doc-surface change with its own review; kept separate so this
  PR stays "proposal + verified reference tools". See **WP-D decision** below.

## Grounded integration points (verified in tree)

The reference tools are faithful to code that exists today; each claim was checked,
not assumed:

- **Classifier input shape** — `db1_execution_trace_mining_row_t`
  (`src/db1/execution_trace.h:66-75`) is exactly
  `{id, plan_id, turn, direction, tool_name, tool_args, tool_result}`. The Python
  reads `{plan_id, turn, direction, tool_name, tool_args, tool_result}` — a subset
  of the same row. **Verified.**
- **Retry-loop rule** — `detect_retry_loops()` (`src/trace_analysis.c:91-139`)
  flags `run >= RETRY_THRESHOLD (3)` consecutive same-tool calls with `errors >= 2`.
  The Python `RETRY_THRESHOLD = 3` / `RETRY_MIN_ERRORS = 2` are kept in lockstep and
  the script says so in a comment. **Verified identical.**
- **Error markers** — the Python `ERROR_MARKERS` tuple is a verbatim copy of the
  substrings in `result_looks_like_error()` (`src/trace_analysis.c:38-52`):
  `error/Error/ERROR`, `failed/Failed/FAILED`, `No such file`, `not found`,
  `Permission denied`, `command not found`. **Verified identical.**
- **Anti-pattern hit-rate** — `src/db2/anti_patterns.h` exposes `int hit_count`
  (`:27`), `db2_anti_pattern_list()` (`:38`), `db2_anti_pattern_bump()` (`:55`).
  `delete_pressure.py`'s `load_anti_patterns()` keys on `hit_count` (with
  `bumps`/`hits` fallbacks) and treats `hit_count == 0` as a removal candidate —
  consistent with "a pattern that has never bumped is dead weight." **Verified.**
- **Delete-pressure target** — `src/tool_prompts/` holds exactly 8 `*.md`
  scaffolds (bash, code_search, grep, list_files, read_file,
  run_background_process, verify, write_file). The static scan runs against the
  real directory. **Verified.**

## Review findings folded (round 1)

1. **The C port cannot run off the reduced `trace_row_t` buffer.** [doc]
   `src/trace_analysis.c` loads each row into an in-memory `trace_row_t` that keeps
   only `int has_error` (`:25`, `:75`) and discards the raw `tool_result`. The
   classifier's `control`/`tool`/`context` sub-classification needs the raw result
   string (for `CONTROL_MARKERS`, `TOOL_FAULT_MARKERS`, and refetch detection), so a
   faithful port **must classify off `db1_execution_trace_mining_row_t`** (which
   retains `tool_result`) or precompute the marker bits at load time. **Resolved:**
   proposal §3 C-port paragraph rewritten to say this, plus a note to lift the bare
   `errors >= 2` into a named `RETRY_MIN_ERRORS` `#define`.

2. **The context classifier missed the canonical "ignored context" case.** [code]
   The original code only raised `redundant-refetch` when the re-issue *itself
   errored*, so a **successful** redundant refetch — the textbook "you re-read what
   was already in your window" failure — produced zero incidents. **Resolved:** the
   classifier now records each success's result bytes and flags a re-issue whose
   result is **byte-identical** to the earlier success (confidence 0.5), in addition
   to the erroring re-issue (0.65). Byte-identity cleanly excludes the legitimate
   read-edit-reread cycle: a real edit changes the bytes, so the re-read is not
   flagged. Proposal §3 and both self-tests updated to match; the dangling
   `# success refetch, see note` comment is gone.

3. **Bare-substring marker matching corrupted the distribution.** [code, bug]
   `CONTROL_MARKERS` / `TOOL_FAULT_MARKERS` were matched as raw substrings, so
   `'oom'` matched `'room-service'`, `'must'` would match `'mustard'`, etc. Because
   the control band *outranks everything*, a false control hit silently misattributes
   a failure. **Resolved:** markers are now compiled with word boundaries on their
   word-character edges (phrase/colon markers like `usage:` and `rate-limit` still
   match), and a self-test asserts a benign `'room'` error is **not** classified as
   control. The same whole-word fix was applied to `delete_pressure.py`'s doubt
   terms (`'only'` no longer matches `'commonly'`), with its own negative self-test.

4. **Self-tests were happy-path only / confounded.** [tests]
   **Resolved:** `classify_failures.py --self-test` now also asserts read-edit-reread
   is not flagged, the `'room'` false-positive guard, the erroring-refetch path,
   empty-trace, all-success (+ the "no failures detected" render path), and
   runaway-length; each assertion fails loudly so a degenerate classifier cannot
   pass. `delete_pressure.py --self-test` now holds word count fixed and varies only
   prescriptiveness (length/doubt no longer confounded), asserts plain prose scores
   zero doubt, and asserts the `commonly`/`mustard`/`preferences` substring trap
   scores zero.

## Review findings folded (round 3 — live roundtable)

Folded (legitimate):

1. **Non-scalar `tool_args` could crash the classifier.** [code] The `(tool, args)`
   dict key assumed `tool_args` is a string (it is in `db1` `char[]`), but the tool
   ingests arbitrary JSON, so a list/dict arg made the key unhashable. **Resolved:**
   a `_key_args()` helper coerces non-scalars to a canonical JSON string; a new
   self-test feeds a list arg and asserts both no-crash and that a true refetch is
   still detected.
2. **`control` did not actually outrank the success-refetch path.** [code] A
   non-error result carrying a control marker (e.g. `deadline exceeded`, which has no
   error word) re-issued byte-identically was tagged `context/redundant-refetch`,
   violating the stated "control outranks everything" invariant. **Resolved:** the
   per-row loop hoists the `consumed` skip and the `CONTROL_PATTERNS` check above both
   the success and refetch branches; a new self-test locks it.
3. **`delete_pressure` turned malformed exports into bulk deletions.** [code] A row
   with none of `hit_count`/`bumps`/`hits` defaulted to `hits=0`, which `render()`
   reads as "never matched = removal candidate" — a schema mismatch would recommend
   deleting every live pattern. **Resolved:** missing count is now `None`
   ("unknown"), kept distinct from a genuine `0`; only present-and-zero rows are
   removal candidates, unknown rows are excluded and flagged as a schema issue.
   `load_anti_patterns` also now uses `with open`, returns `None` on unreadable/
   non-JSON/non-array input, and `main()` exits 2 rather than silently degrading.
   A new self-test asserts the zero-vs-unknown split.
4. **Lockstep coupling was asserted but not pinpointed.** [doc] The `RETRY_THRESHOLD`
   / `RETRY_MIN_ERRORS` comment now cites the exact `src/trace_analysis.c` defines and
   states that, like `detect_retry_loops()`, the run is keyed on `tool_name` only and
   the trace `direction` field is intentionally not a grouping key.

Rejected (panel misread; the deadline-hit/degraded runs produced some spurious
items), with rationale so the boundary is a decision:

- *"runaway-length fires when a control marker appears after the first N rows"* —
  false: the check is `not any(... for r in rows)` over **all** rows, and self-test
  8b already asserts a late control marker suppresses runaway-length.
- *"`rate-limit` vs `rate limit` inconsistency"* — both forms are already listed in
  `CONTROL_MARKERS`.
- *"`\bmust\b` false-matches `mustn't`"* — it does not (`t`→`n` is not a word
  boundary); the only contrived match (`only's`) is not real prose. The remaining
  LOW doc nits are already covered by the docstring's explicit STATIC-vs-RUNTIME and
  "only NOMINATES" caveats.

## Work packets

Each WP is independently verifiable; together they are one `docs(proposal)` PR onto
`docs/four-part-harness-taxonomy` → `testing` (never `main` directly).

### WP-A — proposal document (DONE)
`docs/proposals/pending/four-part-harness-taxonomy.md`. §3 edits applied: the
C-port paragraph now states the port classifies off the mining row that retains
`tool_result` (+ the `RETRY_MIN_ERRORS` `#define` note), and the `redundant-refetch`
bullet now states the byte-identical / error-gated rule and the read-edit-reread
exclusion. No new claims.

### WP-B — classifier reference (DONE)
`scripts/harness/classify_failures.py`. Word-boundary marker matching and
byte-identical successful-refetch detection added (findings 2-3); the self-test was
rebuilt with 11 loud assertions (findings 4). `--self-test` is green, exit 0.

### WP-C — delete-pressure reference (DONE)
`scripts/harness/delete_pressure.py`. Whole-word doubt matching added (finding 3);
self-test rebuilt to separate length from prescriptiveness (finding 4). `--self-test`
green; a plain scan of `src/tool_prompts/` still reports the 8-scaffold inventory,
~207 tok/turn fixed tax, and the same top-3 ranked candidates.

### WP-D — README + index (DONE)
`scripts/harness/README.md` and the `docs/PROPOSALS.md` Pending entry.
**WP-D decision (review, contrarian lens):** do **not** fold the `MANUAL.md`
vocabulary landing (rollout §6.1) into this PR. It is a change to a 85 KB core doc
with its own review surface and is logically the *first rollout step after
acceptance*, not part of shipping the reference tools. Keeping it out holds this PR
to one reviewable claim: "here is the framing and two tools that measure it."

## Verification

- `python3 scripts/harness/classify_failures.py --self-test` → 4/4 PASS, exit 0.
- `python3 scripts/harness/delete_pressure.py --self-test` → PASS, exit 0.
- `python3 scripts/harness/delete_pressure.py` → 8 scaffolds, ~200 tok/turn fixed
  tax, ranked candidates printed.
- `python3 scripts/harness/classify_failures.py --json <trace>` → well-formed
  incidents + distribution; an all-success trace yields "no failures detected".
- Grounding re-checked against `src/trace_analysis.c`,
  `src/db1/execution_trace.h`, `src/db2/anti_patterns.h`, `src/tool_prompts/`.
- **Not verified (honest boundary):** the C ports, the two new subcommands, and the
  A/B harness are design-only and not built or run in this change — matching the
  proposal's own §7. The deciding test for those is an in-process run that this
  environment (no build) cannot perform; they stay "specced, unverified" rather than
  being claimed as working.

## Done-state for the proposal

This PR ships the proposal's **Phase 1 / reference-tooling slice**: the vocabulary,
the subsystem map, and the two verified reference tools. On merge the proposal moves
to `docs/proposals/done/` annotated **"reference tools shipped; C ports + A/B
harness are specced follow-ons"** — the same "shipped phase + named follow-ons"
done-state used across the proposal tree — e.g.
[`optimization-surface.md`](optimization-surface.md) shipped its CLI with
the remainder carried in
[`optimization-surface-residual.md`](optimization-surface-residual.md), and
[`agent-roundtable-authoring-pipeline.md`](agent-roundtable-authoring-pipeline.md)
landed with named deferred follow-ons. The follow-on C work here is tracked by the
proposal's §6 rollout list.
