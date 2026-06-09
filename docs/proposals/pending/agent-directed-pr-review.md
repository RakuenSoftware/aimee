# Proposal: Agent-directed PR review (call in the ensemble with a brief)

- **State:** draft, pending review
- **Author:** JBailes
- **Date:** 2026-06-09
- **Charter roles:** Review (per-round critique and consolidation), Reason
  (saturation/convergence judging). No new role.
- **Scope:** `src/server/delegate_ensemble.c` (thread a caller-supplied `brief`
  into the review-mode prompt builder `build_round_prompt:304`, and return the
  parsed structured review items, not only the consolidated prose, from
  `delegate_roundtable_run`), `src/headers/delegate_ensemble.h` (one new
  `const char *brief` field on `roundtable_opts_t`, and an items array on
  `roundtable_result_t`), `src/server/server_compute.c`
  (`handle_delegate_roundtable:1742` reads `brief` from the request body and
  surfaces the structured items in the run result), a new agent-callable MCP
  tool registered in `src/mcp_tools.c` (beside `delegate:577` /
  `delegate_status:602`) and dispatched in `src/server/server_mcp.c` (beside the
  `strcmp(tool, "delegate"):1396` arm), gated on the existing `CAP_DELEGATE`
  (`server.h:59`). Optional: a `--brief` flag on `aimee delegate roundtable` and
  a thin caller-side `git diff` helper. No new engine, no new role, no new
  capability flag, no new provider.

## Goal

Let an **agent**, mid-task, call in the review-mode roundtable against a code
change and **direct it**: hand the panel a short brief saying what to look at
(the gaps it is unsure about, the fixes it just applied, the invariants it must
not break, the specific questions it wants answered), and get back **structured,
actionable feedback** it can act on, not a wall of prose.

The difference from the review mode that already exists is one word:
**direction**. Today an agent that finishes a non-trivial change throws away the
context no fresh reviewer has. A directed panel starts from the author's own risk
map and either confirms or refutes it, while still catching what the author could
not see.

## Relationship to the roundtable proposal

This builds directly on
[`agent-roundtable-collaborative-drafting.md`](agent-roundtable-collaborative-drafting.md),
specifically its **Mode B (review)**. That proposal generalizes the
Mixture-of-Agents ensemble into a multi-round roundtable and defines the
review-mode feedback contract (severity `blocking|suggestion|nit`, engine-computed
identity keys, saturation-based convergence). That work is **in flight on the
`feat/agent-roundtable` branch** (commit `0fae05d` plus working-tree changes to
`delegate_ensemble.c`); the symbols this proposal cites land with it, not on
`testing` yet. This proposal is **strictly additive** on top of a working review
mode and adds nothing the roundtable needs to function; it can be staged or
dropped without reopening that work. Read the two together: the roundtable is the
engine, this is the agent-facing, directed application of it.

## §0 What already exists (so we do not rebuild it)

The review machinery is built (on the roundtable branch). Confirmed in the tree:

- **The review-mode loop.** `delegate_roundtable_run`
  (`src/headers/delegate_ensemble.h`, impl in `delegate_ensemble.c`) runs the
  review mode end to end: per-round reviewer fan-out, each reviewer routed to a
  distinct configured participant (`agent_run_named`), cost/min-success/degrade,
  consolidation, and saturation-based stop. `roundtable_opts_t.mode ==
  ROUNDTABLE_REVIEW` selects it.
- **The per-round reviewer prompt builder.** `build_round_prompt`
  (`delegate_ensemble.c:304`) already branches on mode and emits the
  "review and critique" framing (`:308`) and the structured-output instruction
  (`:354`). **This is the single seam the brief threads through** (§4); it takes
  no caller context today.
- **The structured-feedback contract.** Reviewers return JSON items; the engine
  parses them in `parse_review_issue_keys` (`delegate_ensemble.c:524`), computes a
  model-independent identity key per item via `normalized_identity_key`
  (`:497`, hashes `category` + `location`, falls back to `summary`), and decides
  saturation with `review_saturated` (`:593`). **But the parse keeps only the
  identity keys for the stop predicate and discards the items themselves** (§5).
- **The reviewer role.** `tasks[i].role = "review"` in review mode
  (`delegate_ensemble.c:617` and the sequential path `:650`); the `review` charter
  role is defined at `src/role_templates.c:21` with severity/category/location
  expectations.
- **The V1 entry point.** `POST /v1/delegate/roundtable` is wired
  (`server_http_routes.inc`, dispatch method `delegate.roundtable` in
  `src/server/server.c`) to `handle_delegate_roundtable`
  (`src/server/server_compute.c:1742`), which loads config, builds
  `roundtable_opts_t`, runs the engine async via `/v1/runs/{id}`, and returns
  `artifact` plus `rounds_run` / `converged` / `degraded` / `truncated` /
  `cost_capped` / `deadline_hit` / `best_round` / `cost_usd`. It is gated
  `CAP_DELEGATE`.
- **The MCP tool pattern.** Agent-callable tools are registered with `build_tool`
  in `src/mcp_tools.c` (`delegate:577`, `delegate_status:602`) and dispatched in
  `src/server/server_mcp.c` (`strcmp(tool, "delegate"):1396` ->
  `handle_mcp_delegate_call:1398`). `CAP_DELEGATE` is `1u << 1` (`server.h:59`).
- **Config.** `ensemble_*` (`config.h:1201-1206`, parser
  `config_sections.c:1068`) supplies participants/aggregator/cost; `roundtable_*`
  (`config.h:1210-1213`, parser `config_sections.c:1104`, CLI-settable scalars
  `config_fields.c:164-171`) supplies the loop defaults. **No new config keys are
  required.**

So the net new code is small and additive: **a brief field threaded into one
prompt builder, the parsed review items returned instead of discarded, and an
agent-callable MCP tool that targets review mode.**

## §1 The gap: review mode is built, but it is undirected and not agent-callable

Two concrete gaps stand between "review mode exists" and "an agent can call in a
directed PR review":

1. **It is undirected.** `build_round_prompt` (`:304`) composes the reviewer
   prompt from `task + artifact + peer_notes` only. The caller cannot say "focus
   on the shutdown race" or "I just bumped the gst pin, confirm it holds." The
   panel re-derives the author's concerns slowly and incompletely, which is
   exactly the context an agent that just wrote the change already has.
2. **It is not reachable by an in-process agent.** The only surfaces are the
   human CLI and the V1 HTTP route. There is no MCP tool an agent can call
   mid-task, and the existing `delegate` MCP tool (`mcp_tools.c:577`) runs a
   single role-routed delegate, not the ensemble or the roundtable. The Explore
   of the tree confirms no `ensemble`/`roundtable` MCP tool exists today.

There is also an **output-shape gap** (§5): even when review mode runs, the
structured items the reviewers produced are parsed only to compute saturation keys
and then dropped; the caller gets back consolidated prose. An agent wants the
items (severity, location, summary) so it can act on each one and re-run.

## §2 The brief: direction without tunnel vision

The brief is a short, optional, freeform-plus-light-structure block the caller
passes in. It carries the four things an author knows that a fresh reviewer does
not:

```json
{
  "focus":      ["concurrency around session shutdown", "socket path handling"],
  "fixes":      ["bumped gst-wayland-display pin", "added graceful stop on SIGTERM"],
  "invariants": ["must keep the /etc/wolf/profile-data bind working"],
  "questions":  ["does lobby teardown race session teardown?"]
}
```

It may also be passed as a single freeform string; the tool accepts either and
normalizes to the block above. Empty brief == today's undirected review, byte for
byte (§4).

**The brief weights attention; it never gates findings.** Every reviewer prompt
appends, verbatim, an instruction of the form: "Prioritize the focus areas and
answer the questions below, but report any blocking issue you find even if it is
outside them." This is the load-bearing rule. The failure mode of a directed
review is tunnel vision: the author points the panel at the wrong place and the
panel misses the real bug. Keeping the mandate open prevents the brief from
suppressing out-of-scope findings; it only re-orders priority and seeds the
`questions` the consolidation must answer.

The brief does **not** change the convergence contract. Saturation still keys on
the engine-computed identity hash (`normalized_identity_key:497`), not on whether
a finding matched the brief. A `questions` block adds one obligation: the
consolidated review must answer each question explicitly (answered / not-answered
+ evidence), so the caller always learns whether its hypothesis was confirmed or
refuted.

## §3 Agent-callable surface: a new MCP tool

Add one MCP tool, the minimum that makes review mode reachable from inside an
agent turn. Working name `ensemble_review` (final name open, §12).

- **Register** in `src/mcp_tools.c` with `build_tool`, immediately after
  `delegate_status` (`:602`). Schema:

  ```json
  {
    "type": "object",
    "properties": {
      "diff":       {"type": "string", "description": "Unified diff / code under review, or a path to it"},
      "brief":      {"type": "string", "description": "Optional: focus areas, fixes made, invariants, questions (freeform or JSON, see proposal)"},
      "rounds":     {"type": "integer", "description": "Max review rounds; default from roundtable.max_rounds"},
      "turns":      {"type": "string", "enum": ["parallel", "sequential"]}
    },
    "required": ["diff"]
  }
  ```

- **Dispatch** in `src/server/server_mcp.c`, a new arm beside
  `strcmp(tool, "delegate"):1396`. The handler builds a `roundtable_opts_t` with
  `mode = ROUNDTABLE_REVIEW`, sets `brief` (§4), folds `rounds`/`turns` over the
  configured defaults, and calls the **same** server-side roundtable path
  `handle_delegate_roundtable` (`server_compute.c:1742`) uses. It must not fork a
  second engine entry; it reuses the one async run path so cancellation
  (`/v1/runs/{id}/stop`), cost cap, and degrade behavior are identical.
- **Gate** on the existing `CAP_DELEGATE` (`server.h:59`). No new capability:
  this is delegate-class, LLM-heavy, cost-bearing work, and `CAP_DELEGATE`
  already fences exactly that. The MCP tool inherits the session's caps the same
  way the `delegate` tool does.

Because the V1 route already exists, the human-facing path (`aimee delegate
roundtable --mode review`) and the new agent-facing MCP path converge on one
implementation. The only new server code is request parsing for `brief` and the
tool dispatch arm.

## §4 Threading the brief through the engine

One seam: `build_round_prompt` (`delegate_ensemble.c:304`).

1. Add `const char *brief;` to `roundtable_opts_t`
   (`delegate_ensemble.h`). Default `NULL`.
2. Pass it through `delegate_roundtable_run` into `build_round_prompt`
   (both the parallel call site `:614` and the sequential `:646`).
3. In `build_round_prompt`, when `mode == ROUNDTABLE_REVIEW` and `brief != NULL`,
   emit the brief block plus the open-mandate instruction (§2) after the
   "review and critique" framing (`:308`) and before the structured-output
   instruction (`:354`). When `brief == NULL` the emitted prompt is unchanged,
   so existing review behavior and its tests are byte-identical.
4. The brief is included in the **per-round** prompt so that in sequential and
   multi-round runs every reviewer (and every later round reacting to peers) keeps
   the same direction. It is **not** included in the consolidation/aggregator
   prompt except for the `questions`, which the consolidator must answer (§2).

Budgeting: the brief counts against the same per-round byte budget the roundtable
already enforces (`build_round_prompt` truncation / summarize-forward path). A
brief that is itself enormous is summarized forward like any other oversized
context, with the existing `truncated` flag set, never silently dropped.

## §5 Structured, actionable output back to the caller

An agent needs the items, not the essay. Today `parse_review_issue_keys`
(`delegate_ensemble.c:524`) extracts identity keys for saturation and discards the
rest. Extend it (or add a sibling that shares the JSON parse) to retain the full
item: `severity`, `category`, `location`, `summary`, plus the engine-computed
identity key.

- Add a bounded items array to `roundtable_result_t` (`delegate_ensemble.h`),
  populated from the **final, consolidated** round (the surviving, deduplicated
  blocking/suggestion/nit set), each item carrying its severity, location,
  one-line summary, and identity key.
- `handle_delegate_roundtable` (`server_compute.c:1742`) serializes that array
  into the run result JSON alongside the existing `artifact` and flags. The MCP
  tool returns the same shape.
- Add `answeredQuestions: [{question, answer, evidence}]` populated by the
  consolidation when the brief carried `questions` (§2), and `coverageGaps`
  (e.g. "no participant addressed the teardown path") when a question went
  unanswered.

This makes the result a thing an agent can loop over: apply each suggested fix,
then re-invoke with `brief.fixes` describing what it just changed, until the panel
saturates. The caller drives that loop; this feature returns findings, it does not
auto-apply them (the roundtable's `apply_review` remains a separate, opt-in final
draft turn).

## §6 PR / diff input

Aimee has **no diff-ingestion machinery** and does not need it for v1: the tree
shells out to `git diff` in places (`src/cmd_slop.c`, `src/mcp_git_write.c`) but
has no PR-fetch or patch-parsing layer. The `diff` argument is plain task text:
the caller (the agent, which already has the working tree and `git`) pastes the
unified diff in. This matches how the roundtable proposal frames its review input
("paste the document under review").

Optional, deferred: a thin caller-side helper that runs `git diff <range>` and
fills `diff` so a human can say `aimee delegate roundtable --mode review --brief
"..." --range origin/main...HEAD`. No server-side git, no GitHub API in v1.
GitHub PR fetch and inline-comment posting are explicit non-goals for v1 (§11);
posting anything outward is opt-in and out of scope here.

## §7 Convergence, cost, and safety are inherited, not rebuilt

Everything that bounds the roundtable bounds this unchanged: the running
`ensemble_max_cost_usd` cap, `roundtable_max_rounds` (default 3),
`roundtable_deadline_ms` (default 600000), `min_successful` degrade-to-best, the
per-round byte budget, and `/v1/runs/{id}/stop` cancellation. The brief and the
agent entry point add no new unbounded loop and no new spend path: the MCP tool is
just another caller of the same async run. The saturation predicate is unchanged
and remains model-independent (engine-computed identity keys), so a directed
review still terminates deterministically.

## §8 Config

No new keys. The brief is per-call (request body / tool argument), not
configuration. Participants, aggregator, cost cap come from `ensemble_*`; loop
bounds from `roundtable_*`. If a deployment wants directed review on by default
there is nothing to toggle: an empty brief is identical to plain review mode.

## §9 Phasing

1. **P1a, brief plumbing.** Add `brief` to `roundtable_opts_t`, thread through
   `build_round_prompt` (§4), parse `brief` in `handle_delegate_roundtable`. The
   open-mandate instruction and the empty-brief == unchanged-prompt invariant
   land here with tests.
2. **P1b, structured return.** Retain and return the parsed review items and
   `answeredQuestions`/`coverageGaps` (§5). This is independently useful for the
   human CLI review mode too.
3. **P1c, agent-callable MCP tool.** Register `ensemble_review` and its dispatch
   arm (§3), `CAP_DELEGATE`-gated, reusing the same run path. This is what
   delivers the headline "an agent can call in the ensemble."
4. **P2, optional ergonomics.** `--brief` CLI flag and the `git diff` range
   helper (§6).

P1a/P1b are mergeable without P1c (they improve the existing review mode); P1c is
the new agent surface. None of them touch the roundtable's core or its P0 fixes.

## §10 Testing

- **Empty-brief parity:** `build_round_prompt` with `brief == NULL` emits a
  byte-identical prompt to today (golden string test); existing review-mode tests
  stay green.
- **Open mandate:** a unit test asserts the brief block is followed by the
  "report blocking issues even if outside focus" instruction, so direction cannot
  be implemented as a filter.
- **Questions answered:** a test with a `questions` brief asserts the result
  carries an `answeredQuestions` entry per question (answered or not), so a
  caller hypothesis is always resolved.
- **Structured return:** a review run returns a non-empty items array with
  severity/location/summary/identity-key for each surviving finding, and the
  identity keys match `normalized_identity_key` (no reliance on model
  `stable_key`).
- **MCP reachability:** an end-to-end test that the `ensemble_review` tool call
  reaches `ROUNDTABLE_REVIEW` (not the single-delegate `handle_mcp_delegate_call`
  path) and requires `CAP_DELEGATE`; a negative test that a session without
  `CAP_DELEGATE` is denied.
- **No second engine:** a negative check that the MCP arm calls the shared
  roundtable run path and does not introduce a parallel inline engine entry.

## §11 Non-goals

- No GitHub integration (PR fetch, status checks, inline comments) in v1; diff is
  caller-provided text. Posting outward is opt-in and out of scope.
- No new role, capability flag, provider, config key, or RPC transport.
- No auto-apply of findings; the caller decides what to act on. (The roundtable's
  own `apply_review` final-draft turn stays separate and opt-in.)
- Not a replacement for the human CLI review mode; it is the agent-facing,
  directed sibling of the same engine.

## §12 Open questions

1. **Tool name.** `ensemble_review` vs `pr_review` vs `review` vs
   `roundtable_review`. `delegate` is taken and means something narrower; the name
   should signal "multi-agent review," not "single delegate."
2. **Brief shape over MCP.** Accept only a freeform string and parse it, or
   accept the structured `{focus, fixes, invariants, questions}` object directly?
   (Leaning: accept both; normalize to the object; questions need structure to
   guarantee per-question answers.)
3. **Items array bound.** What is the cap on returned items, and how do we report
   when a verbose review overflows it (summarize-forward vs drop-with-flag, reuse
   the existing `truncated` semantics)?
4. **Auto-brief.** When an agent does not supply a brief, should the tool
   optionally synthesize one from the diff itself (changed files, churned hunks)
   so even an unbriefed call is lightly directed? (Leaning: no in v1; an empty
   brief must stay identical to plain review mode for parity.)
5. **Aggregator and the questions.** Should answering `questions` be the
   consolidator's job (one extra instruction) or a separate `reason`-role pass?
   (Leaning: consolidator, to avoid an extra LLM call per run.)

## Recommendation

Ship P1a + P1b + P1c: thread an optional `brief` into review mode with an open
mandate, return the structured items plus answered questions, and expose one
`CAP_DELEGATE`-gated MCP tool that targets review mode. It reuses the roundtable
engine, the V1 run path, the review contract, and the existing config and
capability model wholesale; the net new surface is a single prompt-builder seam, a
result array, and one tool. Validate it against a real change with a concrete
invariant and a concrete hypothesis to steer at (the Wolf shutdown-race /
gst-wayland-display work is a good first target: there is an invariant to protect
and a race to ask about).
