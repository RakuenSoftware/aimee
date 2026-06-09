# Proposal: Agent-directed PR review (call in the ensemble with a brief)

- **State:** draft, pending review
- **Author:** JBailes
- **Date:** 2026-06-09
- **Charter roles:** Review (per-round critique and consolidation). Reason runs
  the per-round quality scorer and the draft-mode convergence tiebreak; note that
  review-mode *convergence is deterministic C* (`review_saturated`), not a Reason
  call (§7). No new role.
- **Base:** retargeted at `rakuen/testing` as of `d3eb402d` (the roundtable
  landed via PR #136 and PR #142; the symbols below are live on `testing`, not on
  an in-flight branch). All line numbers are `testing@d3eb402d`; prefer the symbol
  names, which are stable, over the line numbers, which drift.
- **Scope:** `src/server/delegate_ensemble.c` (thread a caller-supplied `brief`
  into the review-mode prompt builder `build_round_prompt:393` **and the two
  functions that call it**, `run_round_parallel:690` / `run_round_sequential:720`;
  retain the parsed structured review items as engine state instead of discarding
  them), `src/headers/delegate_ensemble.h` (one new `const char *brief` field on
  `roundtable_opts_t`, plus a bounded items array and `answeredQuestions` on
  `roundtable_result_t`), `src/server/server_compute.c`
  (`handle_delegate_roundtable:1742` reads `brief` and serializes the items), the
  MCP surface in `src/mcp_tools.c` (register beside `delegate:577` /
  `delegate_status:602`) and `src/server/server_mcp.c` (a new arm beside
  `strcmp(tool, "delegate"):1396` in `handle_mcp_call:1369`), gated on the
  existing `CAP_DELEGATE` (`src/headers/server.h:59`). The MCP surface needs an
  **async bridge** (§3), not just a dispatch arm. Optional: a `--brief` flag on
  `aimee delegate roundtable` and a thin caller-side `git diff` helper. No new
  engine, no new role, no new capability flag, no new provider.

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
identity keys, saturation-based convergence). **That work is now merged to
`testing`** (PR #136, PR #142); the symbols this proposal cites are live, so the
implementation guidance below depends on committed code, not an in-flight branch.
This proposal is **strictly additive** on top of the working review mode and adds
nothing the roundtable needs to function; it can be staged or dropped without
reopening that work. Read the two together: the roundtable is the engine, this is
the agent-facing, directed application of it.

## §0 What already exists (so we do not rebuild it)

The review machinery is built and on `testing`. Confirmed in the tree
(`testing@d3eb402d`):

- **The review-mode loop.** `delegate_roundtable_run`
  (`delegate_ensemble.c:868`, declared in `delegate_ensemble.h`) runs the review
  mode end to end: per-round reviewer fan-out, each reviewer routed to a distinct
  configured participant (`agent_run_named`), cost/min-success/degrade,
  consolidation, and saturation-based stop. `roundtable_opts_t.mode ==
  ROUNDTABLE_REVIEW` selects it.
- **The per-round reviewer prompt builder.** `build_round_prompt`
  (`delegate_ensemble.c:393`) branches on mode and emits the "review and critique"
  framing (`role_hint`, `:396`) and the structured-output instruction
  (`mode_task`, `:397`). **Note the shape: it is a single `snprintf` (`:415-420`),
  not an incremental appender** (contrast `build_synthesis_prompt:327` and
  `build_round_synthesis_prompt:425`, which *are* appenders). It takes no caller
  context today, and there is no statement boundary "between the framing and the
  structured instruction" to insert into; threading the brief is a format-string
  change (§4).
- **The structured-feedback contract.** Reviewers return JSON items; the engine
  parses them in `parse_review_issue_keys` (`delegate_ensemble.c:608`), computes a
  model-independent identity key per item via `normalized_identity_key` (`:581`).
  **It does not hash:** it builds a lowercased, colon-delimited slug from two
  parts - `category` (empty -> `"general"`) and `location`, where `location`
  itself falls back to `summary` only when `location` is empty. So `summary` backs
  the *location component*, not the whole key. Saturation is decided by
  `review_saturated` (`:678`). **Three facts that constrain §5:** (a) the parse
  keeps only the identity keys for the stop predicate and discards the items
  themselves; (b) it counts **blocking** items, but the filter at `:632` only skips
  an item whose `severity` is a *present string* `!= "blocking"` - an item with a
  missing or non-string `severity` is **not** skipped and is counted as blocking,
  which §5 must handle consistently across the saturation set and the new
  all-severity return array; (c) the key arrays are fixed `char[64][128]` stacks,
  so at most 64 keys, silently truncated past that (`:641`).
- **The reviewer role.** In review mode the reviewer role is `"review"`: in
  `run_round_parallel` it is `tasks[i].role = "review"` (`delegate_ensemble.c:704`);
  in `run_round_sequential` there is no `tasks` array - it is passed inline as the
  role argument to `agent_run_named` (`:738`). The `review` charter role is defined
  at `src/role_templates.c:21` with severity/category/location expectations.
- **The V1 entry point.** `POST /v1/delegate/roundtable`
  (`server_http_routes.inc:951`, dispatch `delegate.roundtable` in
  `src/server/server.c:1140`) routes to `handle_delegate_roundtable`
  (`server_compute.c:1742`), which loads config, builds `roundtable_opts_t`, and
  returns `artifact` plus `rounds_run` / `converged` / `degraded` / `truncated` /
  `cost_capped` / `deadline_hit` / `cancelled` / `best_round` / `cost_usd`. It is
  gated `CAP_DELEGATE`. **Crucially it is exposed via `rh_dispatch_op_async`**,
  which is what registers a run and injects `__run_id` (`server_http_routes.inc:404`)
  so `handle_delegate_roundtable` can wire cancellation (`server_compute.c:1770`).
  The handler body itself runs `delegate_roundtable_run` **synchronously**
  (`:1796`); the async behavior comes entirely from the route wrapper.
- **The MCP tool pattern.** Agent-callable tools are registered with `build_tool`
  in `src/mcp_tools.c` (`delegate:577`, `delegate_status:602`) and dispatched in
  `handle_mcp_call` (`src/server/server_mcp.c:1369`, the `delegate` arm at
  `:1396`). Note `build_tool(name, desc, schema)` only **advertises** the tool
  (it takes no caps or handler argument, `mcp_tools.c:12`); dispatch is a separate
  `strcmp` arm in `handle_mcp_call`. There is **no per-tool capability layer** on
  the MCP surface - the only gate is the coarse `mcp.call -> CAP_TOOL_EXECUTE`
  (§3), which is why `CAP_DELEGATE` enforcement for the new tool has to come from
  the dispatch path (§3), not from registration. `CAP_DELEGATE` is `1u << 1`
  (`src/headers/server.h:59`). **`/v1/mcp/call` is routed via *synchronous*
  `rh_dispatch_op` (`server_http_routes.inc:922`), and `handle_mcp_call` never sees
  `__run_id`** - this is the central wiring problem for §3.
- **Config.** `ensemble_*` (`config.h:1202-1206`) supplies
  participants/aggregator/cost; `roundtable_*` (`config.h:1210-1213`, CLI-settable
  scalars `config_fields.c:164-169`) supplies the loop defaults. **No new config
  keys are required.**

So the net new code is small and additive: **a brief field threaded into one
prompt builder (and its two callers), the parsed review items retained instead of
discarded, and an agent-callable MCP tool that targets review mode through an
async bridge.**

## §1 The gap: review mode is built, but it is undirected and not agent-callable

Two concrete gaps stand between "review mode exists" and "an agent can call in a
directed PR review":

1. **It is undirected.** `build_round_prompt` (`:393`) composes the reviewer
   prompt from `task + artifact + peer_notes` only. The caller cannot say "focus
   on the shutdown race" or "I just bumped the gst pin, confirm it holds." The
   panel re-derives the author's concerns slowly and incompletely, which is
   exactly the context an agent that just wrote the change already has.
2. **It is not reachable by an in-process agent.** The only surfaces are the
   human CLI and the V1 HTTP route. There is no MCP tool an agent can call
   mid-task, and the existing `delegate` MCP tool (`mcp_tools.c:577`) runs a
   single role-routed delegate, not the ensemble or the roundtable. No
   `ensemble`/`roundtable` MCP tool exists today.

There is also an **output-shape gap** (§5): even when review mode runs, the
structured items the reviewers produced are parsed only to compute saturation keys
and then dropped; the caller gets back consolidated prose. An agent wants the
items (severity, location, summary) so it can act on each one and re-run.

## §2 The brief: direction without tunnel vision

The brief is a short, optional block the caller passes in. It carries the four
things an author knows that a fresh reviewer does not:

```json
{
  "focus":      ["concurrency around session shutdown", "socket path handling"],
  "fixes":      ["bumped gst-wayland-display pin", "added graceful stop on SIGTERM"],
  "invariants": ["must keep the /etc/wolf/profile-data bind working"],
  "questions":  ["does lobby teardown race session teardown?"]
}
```

Over MCP the brief is accepted as **either** a freeform string **or** this
structured object, and normalized to the object form. `questions` in particular
**must be carried as a structured array, not buried in freeform text**, because
§5 promises a per-question answer back and the consolidator cannot reliably know
which sentences were questions otherwise. The normalization is deterministic: a
freeform string becomes `{focus: [<the string>], questions: []}` (no questions are
invented from prose), so an unstructured brief never silently acquires answer
obligations. Empty brief == today's undirected review, byte for byte (§4).

**The brief weights attention; it never gates findings.** Every reviewer prompt
appends, verbatim, an instruction of the form: "Prioritize the focus areas and
answer the questions below, but report any blocking issue you find even if it is
outside them." This is the load-bearing rule. The failure mode of a directed
review is tunnel vision: the author points the panel at the wrong place and the
panel misses the real bug. Keeping the mandate open prevents the brief from
suppressing out-of-scope findings; it only re-orders priority and seeds the
`questions` the consolidation must answer.

The brief does **not** change the convergence contract. Saturation still keys on
the engine-computed identity slug (`normalized_identity_key:581`), on
blocking-severity items only (§7), not on whether a finding matched the brief.

## §3 Agent-callable surface: a new MCP tool over an async bridge

Add one MCP tool, the minimum that makes review mode reachable from inside an
agent turn. Working name `ensemble_review` (final name open, §12).

- **Register** in `src/mcp_tools.c` with `build_tool`, immediately after
  `delegate_status` (`:602`). Schema:

  ```json
  {
    "type": "object",
    "properties": {
      "diff":       {"type": "string", "description": "Unified diff / code under review (caller-provided text, min 20 chars)"},
      "brief":      {"type": ["string", "object"], "description": "Optional: {focus, fixes, invariants, questions} object, or freeform string (see §2)"},
      "rounds":     {"type": "integer", "description": "Max review rounds; default from roundtable.max_rounds"},
      "turns":      {"type": "string", "enum": ["parallel", "sequential"]}
    },
    "required": ["diff"]
  }
  ```

  `diff` is **caller-provided text only** - no path argument, no filesystem read,
  no PR fetch (§6, §11). It maps to the engine's `task` argument and therefore
  inherits the existing `handle_delegate_roundtable` validation, including the
  **20-character minimum** (`server_compute.c:1749`); a one-line hunk below that
  floor is rejected with "roundtable prompt too short," so the tool description
  states the minimum.

- **Dispatch and lifecycle - the load-bearing detail.** The proposal cannot just
  call `handle_delegate_roundtable` inline from the MCP arm. `/v1/mcp/call` is
  routed via **synchronous** `rh_dispatch_op`, and `handle_mcp_call` never
  receives `__run_id`. Calling the handler inline would:
  1. **Block the MCP request for up to `roundtable_deadline_ms`** (default
     600000 ms = 10 minutes) while the full multi-round loop runs on the request
     thread, and
  2. **Silently lose cancellation**: with no `__run_id`, `opts.cancel_requested`
     stays `NULL`, every cancel check in `delegate_roundtable_run` is dead, and
     `/v1/runs/{id}/stop` has no run to stop. The cost cap and deadline still
     apply (they read config, not the run), but "cancellation identical to the V1
     route" is **not** achievable by inline reuse.

  So `ensemble_review` must launch through the **same async op-run machinery the
  V1 route uses** - i.e. submit `delegate.roundtable` as an async run so a run id
  and `__run_id` are created - then **return that run id immediately** and let the
  agent poll. Concretely, `ensemble_review` returns `{run_id, status: "running"}`
  and the agent polls `/v1/runs/{id}` (or a thin `ensemble_review_status` sibling)
  for the structured result. The UX precedent is the `delegate` MCP tool
  (`server_mcp.c:1396-1410`): it starts a background job and returns an id;
  `delegate_status` polls. **But mirror its *UX*, not its dispatch.** (See the next
  bullet: the `delegate` tool reaches its worker by a direct C call that *bypasses
  the method-cap gate*, which is precisely the capability hole `ensemble_review`
  must avoid.)

- **What the async machinery actually is, and what "extract a shared submit
  helper" really means.** `rh_dispatch_op_async` is **`static` in
  `server_http_routes.inc`** (`:386`) - file-local, declared in no header, so it
  is not linkable from `server_mcp.c` as-is. Its signature is
  `(const route_req_t *rq, char *resp, int cap)` - it does **not** take a handler
  function pointer, a parsed request, or a caps argument (an earlier draft framed
  it that way; that is wrong). It works by three indirections, each of which the
  bridge must invert:
  1. **Handler is implicit via re-dispatch.** It re-serializes `rq->body` with
     `method` set to `rq->op` and runs it through `loopback_rpc` ->
     `server_dispatch` on a worker thread (`op_run_worker:338`, `loopback_rpc` at
     `server_http.c:915`). It never calls `handle_delegate_roundtable` directly.
     So the bridge submits the *method string* `"delegate.roundtable"`, not a
     function pointer.
  2. **`__run_id` lives in the body.** The run id is generated, then injected into
     the request JSON (`server_http_routes.inc:404-405`) before dispatch; that is
     the only reason `handle_delegate_roundtable:1770` can wire cancellation. The
     bridge must preserve this exact "`__run_id`-in-body" convention.
  3. **Caps come from a thread-local, copied at enqueue.** The worker dispatches
     with `j->conn_caps`, captured from the `_Thread_local g_rpc_conn_caps`
     (`server_http.c:907`) at submit time (`:429`). That thread-local is populated
     on the HTTP listener thread; on the MCP dispatch path it is **not** guaranteed
     to hold the caller's caps. So the bridge must thread the caller's real caps
     (`conn->capabilities` from the MCP call) **explicitly** into the job, not
     rely on the thread-local.

  Net: P1c must factor the body of `rh_dispatch_op_async` (run-id generation,
  `__run_id` injection, `openai_runs_store_create`, `pthread_create(op_run_worker)`)
  into a helper taking `(op_method, request_json, caps)` and returning the queued
  run snapshot / run id, callable from both the route layer and `server_mcp.c`.
  This also means **moving/exporting** `op_run_job_t`, `op_run_worker`, and
  `op_run_snapshot`, all `static` in the `.inc` today. Note there is already a
  *second*, parallel copy of this async-run pattern in `openai_chat.c` (its own
  worker plus `openai_runs_store_create`/`finalize`); the right move is to extract
  one shared helper, not add a third copy.

- **Gate on `CAP_DELEGATE` - and get it for free by routing through the method
  gate.** Capability checks are keyed on the JSON-RPC **method** string, once, in
  `server_dispatch` (`server.c:1296`, via `server_capability_for_method`); the
  registry maps `delegate.roundtable -> CAP_DELEGATE` (`server_auth.c:84`) and
  `mcp.call -> CAP_TOOL_EXECUTE` (`server_auth.c:106`). Two consequences:
  1. **The existing `delegate` MCP tool does *not* enforce `CAP_DELEGATE` today.**
     It reaches `handle_delegate` by a direct C call (`handle_mcp_delegate_call`
     -> `handle_delegate`), already inside the `mcp.call`/`CAP_TOOL_EXECUTE` gate,
     so the `delegate -> CAP_DELEGATE` registry entry is never consulted. A
     principal holding only `CAP_TOOL_EXECUTE` can drive a delegate over
     `mcp.call`. This is a pre-existing latent privilege gap; `ensemble_review`
     must not reproduce it.
  2. **The async-submit design closes the gap automatically.** Because
     `op_run_worker` re-dispatches through `loopback_rpc` -> `server_dispatch`
     with a fake connection whose `capabilities` are `j->conn_caps`
     (`server_http.c:931`), submitting method `"delegate.roundtable"` **re-runs the
     per-method cap gate** against those caps. So if the bridge threads the MCP
     caller's `conn->capabilities` into the job, `CAP_DELEGATE` fires on its own -
     **no separate explicit check is needed on this path.** An explicit
     `CAP_DELEGATE` check is required only as a fallback if any direct
     (non-re-dispatched) handler path is kept. Either way, add the negative test
     (a `CAP_TOOL_EXECUTE`-only session is denied).

Because the V1 route already exists, the human-facing path (`aimee delegate
roundtable --mode review`) and the new agent-facing MCP path converge on one
engine and one async run lifecycle.

## §4 Threading the brief through the engine

The seam is `build_round_prompt` (`delegate_ensemble.c:393`), but because it is a
single `snprintf`, the change is to its **format string and signature**, not an
appended block:

1. Add `const char *brief;` to `roundtable_opts_t` (`delegate_ensemble.h`).
   Default `NULL`.
2. Thread `brief` through the **two callers** the proposal must not skip -
   `run_round_parallel` (`:690`, call site `:701`) and `run_round_sequential`
   (`:720`, call site `:733`) - into `build_round_prompt`. Both wrapper signatures
   gain the parameter; `delegate_roundtable_run` already holds `local.brief`.
3. In `build_round_prompt`, when `mode == ROUNDTABLE_REVIEW`, render the brief
   into a **middle `%s`** that is the empty string when `brief == NULL`, placed
   after the role framing and before the `ROUND %d INSTRUCTION` block, carrying the
   open-mandate sentence (§2). When `brief == NULL` (or in draft mode) the `%s`
   expands to nothing and the surrounding literal is unchanged, so the emitted
   prompt is **byte-identical** to today and existing review-mode tests stay green
   (§10).
4. The brief is included in the **per-round** prompt so every reviewer (and every
   later round reacting to peers) keeps the same direction. It is **not** injected
   into the consolidation prompt, except that `questions` are handed to the
   structured-answer pass (§5).

**Budgeting (corrected).** The roundtable does *not* bound `build_round_prompt`
output to a fixed per-round byte budget - it allocates dynamically by input length
(`:411-412`). The only summarize-forward path is in `delegate_roundtable_run`,
gated on `strlen(artifact) + strlen(peer_notes) + strlen(task) > 22000`
(`:931`), and the brief is none of those three, so **adding the brief does not
automatically count against that check or set `truncated`.** This proposal
therefore defines an explicit bound: the normalized brief is capped at **4 KB**;
input beyond that is truncated at the seam and `roundtable_result_t.truncated` is
set, so an oversized brief is never silently dropped and never silently inflates
the prompt. (A brief is author-supplied direction, not document content;
summarize-forward is the wrong tool for it.)

## §5 Structured, actionable output back to the caller

An agent needs the items, not the essay. Today `parse_review_issue_keys`
(`delegate_ensemble.c:608`) extracts identity keys for saturation and discards the
rest. The fix is to **retain the full parsed items as engine state at parse
time** - *not* to re-shape the aggregator's consolidated prose.

Why this distinction matters: the aggregator's consolidated response is reused
**verbatim** as the next round's `artifact` and `peer_notes`
(`delegate_ensemble.c:1087`, `:1095`), and `peer_notes` is fed straight into the
next reviewer prompt (`:417`). If we satisfied the structured contract by forcing
the **aggregator** to emit JSON, that JSON would become the next round's peer
notes and change reviewer behavior and the saturation dynamics. So the consolidation
channel stays prose; the machine-readable findings come from the participant parse.

- Extend the parse (or add a sibling sharing the JSON walk) to retain each item's
  `severity`, `category`, `location`, `summary`, plus the engine-computed identity
  key, into a bounded array on `roundtable_result_t` (`delegate_ensemble.h`).
- **Severity scope is split deliberately.** The returned array carries
  `blocking | suggestion | nit` (the agent wants all of them to act on), but
  **saturation/convergence stays blocking-only and unchanged** (`:632`, `:678`).
  Widening the returned set must not widen the stop predicate. Concretely: keep the
  existing blocking-key extraction for `review_saturated`; add a *separate*
  all-severity item buffer for the result. They share the JSON parse but not the
  filter.
- **Which round's items, and capture *before* the per-round free.** Populate from
  the **final round that ran** (the surviving per-participant findings of that
  round, deduped by identity key across participants), since that is the set the
  panel last stood behind. Note this is pre-consolidation participant output,
  deduped - not "the consolidated round," which is prose and has no items array.
  **The capture point is load-bearing:** `results[i].response` is a round-scoped
  allocation (declared fresh each iteration, `delegate_ensemble.c:940-941`) and is
  freed inside the loop body at multiple exits - the main synthesis path frees all
  responses at `:1062-1063`, with other frees on the degraded branch (`:1052`),
  the cost-cap branch (`:1024`), and repair/error paths (`:950`, `:1005`, `:1039`).
  **No participant items survive to the end of `delegate_roundtable_run` today** -
  only the consolidated `artifact`/`peer_notes` and the `char[128]` identity-key
  slugs do. So the items must be deep-copied at parse time, before whichever free
  the final round exits through. This is exactly why §5's "retain at parse time"
  matters and ties to finding #5 below: a run that exits via cost cap after
  fan-out, `successful < min_ok` degrade, cancellation, or deadline must still
  populate the items it had (and set `truncated`/a partial flag), not return an
  apparently complete-but-empty list.
- **Bound.** The existing key path caps at **64** and truncates silently
  (`:641`). The items array reuses that bound; on overflow it sets `truncated`
  (rather than dropping silently) so the caller knows the list is partial. Raising
  the cap is a deliberate, separate decision (§12 Q3).
- Add `answeredQuestions: [{question, answer, evidence}]` for each `questions`
  entry the brief carried, and `coverageGaps` when a question went unanswered.
  Because the consolidation is prose, the per-question answers come from a small
  **structured pass over the consolidated review** keyed on the
  `questions` array (one extra instruction to the consolidator, or a single
  `reason`-role JSON pass - §12 Q5), not from parsing free prose. An empty
  `questions` array means no `answeredQuestions` obligation.

This makes the result a thing an agent can loop over: apply each suggested fix,
then re-invoke with `brief.fixes` describing what it just changed, until the panel
saturates. The caller drives that loop; this feature returns findings, it does not
auto-apply them (the roundtable's `apply_review` remains a separate, opt-in final
draft turn).

**Convergence semantics the caller must know.** `review_saturated` (`:678-688`)
returns "converged" under **either** of two conditions: (a) the current round
produced **zero blocking** keys (`cur_count <= 0`, `:680`), or (b) every current
blocking key was **already seen in the previous round** (no new blocking finding,
`:684-687`). So a directed review whose brief has steered reviewers onto
already-fixed code - only suggestions/nits remain - **converges in a single round**
via (a) and reports `converged: true`, even though the returned items array is
non-empty (those non-blocking items did not gate convergence); and a review whose
blocking set stops growing converges via (b). The agent's apply-and-re-run loop
should treat `converged` as "no *new* blocking issues remain," not "no items
remain," and decide independently whether to act on the returned suggestions/nits.
This blocking-only definition is intentional and inherited; the proposal does not
change it.

## §6 PR / diff input

Aimee has **no diff-ingestion machinery** and does not need it for v1: the tree
shells out to `git diff` in places (`src/cmd_slop.c`, `src/mcp_git_write.c`) but
has no PR-fetch or patch-parsing layer. The `diff` argument is plain task text:
the caller (the agent, which already has the working tree and `git`) pastes the
unified diff in. This matches how the roundtable proposal frames its review input
("paste the document under review"). The schema therefore takes **text only** - no
path argument (an earlier draft said "or a path to it"; that is dropped, since a
path would require filesystem reads, workspace binding, detached/remote-workspace
checks, and payload limits that are all out of scope for v1).

Optional, deferred: a thin caller-side helper that runs `git diff <range>` and
fills `diff` so a human can say `aimee delegate roundtable --mode review --brief
"..." --range origin/main...HEAD`. No server-side git, no GitHub API in v1.
GitHub PR fetch and inline-comment posting are explicit non-goals for v1 (§11).

## §7 Convergence, cost, and safety are inherited, not rebuilt

Everything that bounds the roundtable bounds this unchanged: the
`ensemble_max_cost_usd` cap, `roundtable_max_rounds` (default 3),
`roundtable_deadline_ms` (default 600000), `min_successful` degrade-to-best, the
summarize-forward path, and - **once the async bridge of §3 is in place** -
`/v1/runs/{id}/stop` cancellation. The saturation predicate is unchanged and
remains model-independent (engine-computed identity keys), so a directed review
still terminates deterministically. Three accuracy notes for implementers:

- **The cost cap is post-round, not preflight-binding.** The per-round preflight
  estimate only **logs a warning and continues** (`delegate_ensemble.c:922-930`);
  the cap is enforced after a round's observed cost (`:1013`, `:1120`). An agent
  cannot rely on preflight to prevent the first expensive round.
- **A `reason`-role quality scorer runs every round in review mode**
  (`run_quality_scorer:477`, called at `:1042` and `:1110`), even though review
  convergence is decided by the deterministic `review_saturated`, not by that
  score (the score only selects `best_artifact`). That is an extra `reason` LLM
  call per round; surfaced here as expected cost. Skipping the scorer in review
  mode is a possible optimization (it would also remove a non-determinism source
  from an otherwise deterministic review) but is out of scope.
- **Malformed participant JSON triggers a repair call.** `repair_review_json`
  (`:648`) re-invokes **that participant's own reference model**
  (`ensemble_reference_models[participant]`, role `"review"`, not a generic
  reference model) to fix bad JSON, an extra call per malformed participant per
  round. An agent that loops `ensemble_review` will hit this more often than the
  human CLI; no new spend *path*, but a heavier per-call spend profile worth
  budgeting for.

## §8 Config

No new keys. The brief is per-call (request body / tool argument), not
configuration. Participants, aggregator, cost cap come from `ensemble_*`; loop
bounds from `roundtable_*`. If a deployment wants directed review on by default
there is nothing to toggle: an empty brief is identical to plain review mode.

## §9 Phasing

1. **P1a, brief plumbing.** Add `brief` to `roundtable_opts_t`; thread through
   `run_round_parallel` / `run_round_sequential` into `build_round_prompt` as the
   empty-when-NULL middle `%s` (§4); parse `brief` (string-or-object) in
   `handle_delegate_roundtable`; enforce the 4 KB cap. The open-mandate instruction
   and the byte-identical empty-brief invariant land here with tests.
2. **P1b, structured return.** Retain the parsed review items as engine state
   (all severities for the result, blocking-only for saturation) and add
   `answeredQuestions`/`coverageGaps` (§5). Independently useful for the human CLI
   review mode too.
3. **P1c, agent-callable MCP tool over the async bridge.** Register
   `ensemble_review`, add the async submit + run-id return + poll surface (§3),
   `CAP_DELEGATE`-gated, reusing the V1 run lifecycle. This is the headline "an
   agent can call in the ensemble." **It depends on the async bridge, not a bare
   dispatch arm.**
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
- **Brief cap:** a brief over 4 KB is truncated at the seam and sets `truncated`;
  it is never silently dropped and never pushed through summarize-forward.
- **Questions answered:** a test with a structured `questions` brief asserts the
  result carries an `answeredQuestions` entry per question (answered or not), and
  a freeform-string brief acquires **no** answer obligation.
- **Severity split:** a review run returns an items array containing
  `suggestion`/`nit` entries while `review_saturated` still keys only on
  `blocking` (a run with zero blocking items converges in one round yet still
  returns the non-blocking items).
- **Missing/non-string severity:** an item whose `severity` field is absent or
  non-string is counted as blocking by the existing filter (`:632`); a test pins
  how it appears in the returned all-severity array (e.g. normalized to
  `blocking`) so the saturation set and the return array stay consistent.
- **Degraded-exit capture:** a run that exits before consolidation (cost cap
  after fan-out, `successful < min_ok`, cancellation, deadline) still returns the
  participant items it had and sets the partial flag, never an empty-but-complete
  list.
- **Structured return / identity keys:** each returned item carries
  severity/location/summary and an identity key that matches
  `normalized_identity_key` (no reliance on a model-supplied stable key); overflow
  past 64 sets `truncated`.
- **MCP reachability + async:** an end-to-end test that `ensemble_review` reaches
  `ROUNDTABLE_REVIEW` (not the single-delegate `handle_mcp_delegate_call` path),
  returns a run id without blocking the request, and supports `/v1/runs/{id}/stop`.
- **CAP_DELEGATE via the method gate:** a session holding `CAP_TOOL_EXECUTE` but
  **not** `CAP_DELEGATE` is denied when it calls `ensemble_review` - proving the
  cap is enforced by the re-dispatched `delegate.roundtable` method gate
  (`server.c:1296`) and not merely inherited from the `mcp.call`/`CAP_TOOL_EXECUTE`
  gate. (Contrast: the same session *can* drive the existing `delegate` MCP tool
  today, which is the pre-existing hole this design avoids.)
- **No second engine:** a negative check that the MCP path drives the shared
  async roundtable run and does not introduce a parallel inline engine entry.

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
2. **Async surface shape.** Return a bare `run_id` and let the agent poll
   `/v1/runs/{id}` (reusing the existing run surface), or add a dedicated
   `ensemble_review_status` tool mirroring `delegate_status`? (Leaning: reuse
   `/v1/runs/{id}` to avoid a second polling tool; confirm the run-result JSON
   carries the new items array.)
3. **Items array bound.** The engine's de-facto cap today is **64** with silent
   truncation (`:641`). Keep 64 and set `truncated` on overflow (this proposal's
   default), or raise it deliberately for verbose reviews? Raising it means heap
   allocation instead of the current fixed stack arrays.
4. **Auto-brief.** When an agent does not supply a brief, should the tool
   optionally synthesize one from the diff itself (changed files, churned hunks)?
   (Leaning: no in v1; an empty brief must stay identical to plain review mode for
   parity.)
5. **Answering the questions.** Should the per-question answer pass be one extra
   instruction folded into the consolidator, or a separate `reason`-role JSON pass?
   (Leaning: fold into the consolidator to avoid an extra LLM call, accepting that
   the consolidator must then emit a small side JSON the engine reads without
   feeding it back into `peer_notes`.)

## §13 Second-pass verification findings (implementation gaps)

A second review pass re-validated every symbol and line cited above against
`testing@d3eb402d` (all cited files are byte-identical to that commit) and
confirmed them. It also surfaced six concrete implementation gaps the sections
above do not yet cover. Each is small but load-bearing; fold them into the
phase they belong to.

1. **`build_round_prompt`'s malloc must grow by the brief length (P1a).** §4
   correctly notes the prompt buffer is sized dynamically, but the size
   expression is `needed = strlen(task) + strlen(artifact) + strlen(peer_notes)
   + 1200` (`delegate_ensemble.c:411-412`), and that `1200` is fixed slack for
   the *template literal + round number only*. Threading the brief as a new
   middle `%s` without adding `strlen(brief)` to `needed` makes the final
   `snprintf` (`:415-420`) silently truncate — and because the brief sits
   *before* the closing `ROUND %d INSTRUCTION:\n%s` block, the part that gets
   cut is the structured-output contract (`mode_task`), which breaks JSON
   parsing for that reviewer and forces a `repair_review_json` call (or a
   degrade). P1a must add the (capped) brief length into `needed`, not only
   enforce the 4 KB cap. Add a test: a near-4 KB brief still emits a complete,
   parseable prompt with the trailing instruction intact.

2. **Registering the MCP tool breaks the tools-list golden — regenerate it
   (P1c).** `ensemble_review` is added via `build_tool` in
   `mcp_build_tools_list()`, which is pinned by an auto-generated surface net:
   `src/tests/test_mcp_tools_golden.inc` (`MCP_TOOLS_GOLDEN_COUNT 86`, one
   sorted `name {props} req:...` line per tool), asserted by
   `test_mcp_client_registry`. P1c will fail CI on the count and the new line
   until the golden is regenerated (`DUMP_TOOLS=1
   ./unit-test-mcp-client-registry 2>&1`, per the file header). §10's test
   list should name this regeneration as a required step, since it is a
   guaranteed red build otherwise.

3. **The shared async-submit helper inherits a single-writer run-id counter
   (P1c).** `rh_dispatch_op_async` mints run ids with a non-atomic
   `static unsigned long g_op_run_seq` (`server_http_routes.inc:400`) annotated
   "listener thread only; single-writer." §3's extracted
   `(op_method, request_json, caps)` helper is now reachable from a second
   call site (the MCP dispatch path). The helper must preserve that
   single-threaded-submit invariant, or make the counter atomic, otherwise two
   concurrent submits can mint a colliding id (and `openai_runs_store_create`
   would key two runs to one id). Confirm the MCP arm enqueues on the same
   listener thread, or switch the counter to an atomic/locked allocator as part
   of the extraction.

4. **The bridge must rename the `diff` argument to `prompt` (P1c).** The tool
   schema names the input `diff`, but the re-dispatched `delegate.roundtable`
   handler reads `prompt` and validates *that* field for the 20-char minimum
   (`handle_delegate_roundtable`, `server_compute.c:1745-1753`). §3/§6 say "diff
   maps to the engine's task," but the concrete step is: the async-submit bridge
   must copy `args.diff` into a `prompt` field on the request body it builds
   before submitting `delegate.roundtable`. If it forwards `diff` verbatim, the
   handler sees an empty `prompt` and rejects with "missing prompt" rather than
   the intended short-input message. State the rename explicitly.

5. **Returned-item dedup by identity key can drop distinct actionable findings
   (P1b).** §5 dedups the returned items "by identity key across participants,"
   reusing `normalized_identity_key` — which keys on `category` + `location`
   only (`summary` backs the location component *only when location is empty*,
   `delegate_ensemble.c:586-587`). That is correct for the blocking-saturation
   set (the question there is "did a *new class* of blocking issue appear"), but
   for the agent-facing actionable array it means two genuinely different issues
   at the same `location` with the same `category` collapse to one returned
   item and the other is silently dropped. The returned array should dedup on a
   key that also includes `summary` (or carry both `severity`+`summary` so
   distinct findings survive), kept separate from the saturation key. Add a test
   with two same-location/same-category but different-summary items asserting
   both survive in the returned array while the saturation set still counts one
   key.

6. **The pre-existing `delegate` MCP capability hole deserves a tracked
   side-fix, not just avoidance.** §3 proves `handle_mcp_delegate_call` reaches
   `handle_delegate` by a direct C call (`server_mcp_delegate.c:65`) inside the
   `mcp.call`/`CAP_TOOL_EXECUTE` gate, so the `delegate -> CAP_DELEGATE` registry
   entry (`server_auth.c:82`) is never consulted — a `CAP_TOOL_EXECUTE`-only
   principal can drive a delegate today. `ensemble_review` correctly avoids
   reproducing this by routing through the re-dispatched method gate, but the
   existing hole stays open. Since this pass has pinpointed it exactly, fold a
   defense-in-depth explicit `CAP_DELEGATE` check into `handle_mcp_delegate_call`
   (with a negative test) as a small tracked side-fix, rather than leaving a
   known privilege gap next to the new, correctly-gated sibling.

   Related lifetime note for §12 Q3: if the items array / `answeredQuestions`
   move to heap (the "raise the cap" option), `delegate_roundtable_result_free`
   (`delegate_ensemble.h:64`) must free them too, or each run leaks. The default
   fixed-stack bound avoids this; only the heap variant needs the free path
   extended.

## §14 Third-pass and independent verification findings

A further pass re-read the design against `testing@d3eb402d` and verified every
symbol below in the tree. It splits into (A) third-pass bridge/contract gaps that
had been raised in PR discussion but were not yet folded into this document, and
(B) net-new findings from an independent verification that the earlier passes did
not surface. §13 above is the *second* pass; this section is the third pass plus
the independent check.

### §14.A Bridge and contract gaps (fold into P1c unless noted)

1. **The MCP bridge must force `mode: "review"`.** `handle_delegate_roundtable`
   defaults to `ROUNDTABLE_DRAFT` (`server_compute.c:1764`) and only switches to
   review when the submitted body carries `mode: "review"` (`:1777-1779`). The
   `ensemble_review` schema has no `mode` argument, so the async-submit bridge must
   **synthesize `mode: "review"`** into the re-dispatched `delegate.roundtable`
   body. Otherwise the tool launches the *draft* loop against a diff and never
   parses or returns review items. Keep `apply` absent/false on this path
   (`apply_review` defaults to 0 via the handler `memset`, `:1786-1788`) unless an
   auto-apply feature is deliberately added.

2. **The queued run shape is `id`, not `run_id`.** `op_run_snapshot` emits
   `{id, object:"op.run", method, status, created, result?}`
   (`server_http_routes.inc:323-327`), while §3 advertises `{run_id,
   status:"running"}`. Either the MCP arm translates `id` into a `run_id` field for
   tool callers, or the contract adopts the existing `id` field. Pin a test that the
   returned identifier is exactly the one accepted by `GET /v1/runs/{id}`.

3. **The initial run status is `queued`, not `running`.** The snapshot is created
   with status `queued` (`server_http_routes.inc:411`); `op_run_worker` then sets
   the store to `in_progress` (`:341`) and finalizes as `completed|failed|cancelled`
   (`:374-378`). The store never emits `running`. If the tool promises
   `status:"running"`, translate it intentionally or adopt the store's
   `queued|in_progress|completed|failed|cancelled` vocabulary so callers do not
   special-case a value that is never produced.

4. **Polling and cancellation need capabilities beyond `CAP_DELEGATE`.** The
   follow-up lifecycle routes are gated separately: `GET /v1/runs/{id}` and
   `/v1/runs/{id}/events` require `CAP_SESSION_READ`, and `POST /v1/runs/{id}/stop`
   requires `CAP_CHAT` (`server_http_routes.inc:972-974`). A principal holding
   `CAP_TOOL_EXECUTE|CAP_DELEGATE` but neither of those can *start* a review yet
   cannot *read or stop* it. Resolve by documenting the additional required caps,
   re-gating the op-run lifecycle routes, or adding MCP-native status/cancel
   siblings that enforce the intended delegate capability.

5. **Brief truncation needs a threaded flag, not just a reserved bit.** §4 sets
   `roundtable_result_t.truncated` on a >4 KB brief, but if normalization/truncation
   happens in `handle_delegate_roundtable` *before* the engine runs, the engine has
   no way to know the brief was truncated unless that fact is threaded in
   (`opts.brief_truncated`, a normalized-brief struct, or a post-run response
   patch). Otherwise brief truncation is silent while `result.truncated` stays
   reserved for the summarize-forward overflow path. Add a direct handler/HTTP test,
   not only a prompt-builder unit test.

6. **Tests should pin the synthesized delegate body, not just end behavior.**
   Several of the above are single-field bridge mistakes (`diff`->`prompt`, add
   `mode:"review"`, preserve `brief`, copy the caller's caps, omit `apply`). Add a
   narrow unit seam around the MCP->`delegate.roundtable` request construction so
   these fields are asserted *before* the async worker runs; the end-to-end async
   test can then focus on run lifecycle and cancellation.

### §14.B Net-new findings (independent verification)

1. **The MCP result is unwrapped, and the async helper writes to a `resp` buffer,
   not to `conn`.** The existing `delegate` MCP tool reaches `handle_delegate`,
   which writes its response straight to the connection via `server_send_ok`
   (`server_mcp_delegate.c:65`). But `rh_dispatch_op_async` writes the queued
   snapshot into the **route-layer `resp` char buffer** — its signature is
   `(const route_req_t *rq, char *resp, int cap)` (`server_http_routes.inc:386`),
   with no `server_conn_t`. So the extracted helper must **return** the run id /
   snapshot as a value, and the `server_mcp.c` arm must itself `server_send_ok`
   that `{run_id|id, status}` onto `conn`. State explicitly that the MCP arm owns
   the connection write; the shared helper must not assume a route-layer `resp`
   buffer or a particular response envelope.

2. **`g_rpc_conn_caps` *is* correctly populated on the synchronous `mcp.call`
   path — frame the explicit-threading guidance as robustness, not a live bug.**
   §3 bullet 3 says the thread-local "is not guaranteed to hold the caller's caps."
   In fact `g_rpc_conn_caps` is set per request on the listener thread
   (`server_http.c:1655`), and `/v1/mcp/call` dispatches **synchronously on that
   same thread** via `rh_dispatch_op`, so by the time `handle_mcp_call` runs the
   thread-local already equals the MCP caller's caps. Threading
   `conn->capabilities` explicitly is still the right design — it is correct under
   any future nested or worker-thread re-dispatch of `mcp.call`, where the
   thread-local could diverge — but the rationale is **defense-in-depth /
   correctness-under-nesting**, not a bug on the common path. The negative
   `CAP_DELEGATE` test still proves the gate either way.

3. **`turns: "parallel"` is inert against a sequential config default.**
   `handle_delegate_roundtable` only forces `ROUNDTABLE_SEQUENTIAL` when the body
   `turns == "sequential"` (`server_compute.c:1780-1782`); it never forces
   `ROUNDTABLE_PARALLEL`. So if `roundtable_turns` config is `"sequential"`, a tool
   caller passing `turns:"parallel"` still runs sequentially. The schema advertises
   an enum value the handler does not honor symmetrically. Either make the handler
   honor `"parallel"` explicitly, or document `turns` as "request *sequential*;
   otherwise inherit the config default" so the schema does not over-promise.

4. **`answeredQuestions`/`coverageGaps` need a free path independent of the
   items-array bound decision.** §13.6's lifetime note ties extending
   `delegate_roundtable_result_free` (`delegate_ensemble.h:64`) to the heap "raise
   the cap" variant of the items array (§12 Q3). But `answeredQuestions`
   (`{question, answer, evidence}`) is inherently variable-length text and cannot
   live in a fixed `char[N]` stack the way the 64×128 identity keys do; it will be
   heap-allocated even in the **default** P1b shape. So the result-free path must
   free `answeredQuestions`/`coverageGaps` regardless of whether the items array
   stays fixed-stack. Add this to P1b's free path and its leak test, not only to the
   Q3 heap variant.

5. **The enlarged result must fit `SHTTP_RESP_MAX` (256 KB), or the async run
   fails with a generic bridge error.** The async worker captures the
   re-dispatched handler response into a `SHTTP_RESP_MAX` buffer (256 KB,
   `server_http.c:40`) via `loopback_rpc` (`server_http_routes.inc:343,355`).
   `loopback_rpc` validates the captured body before returning; if truncation
   makes it malformed, it overwrites the buffer with `{"error":"rpc response too
   large or malformed"}` and returns 502 (`server_http.c:947-951`). So the current
   failure mode is **not** an opaque string result; it is a failed run whose
   `result` contains the bridge error object. That is better than silent
   corruption, but still too opaque for a caller trying to act on review items.
   In practice a consolidated review (~16 KB) plus 64 short items (~20 KB) is well
   under 256 KB, so this is a robustness note, not a likely failure: bound the
   serialized items/`answeredQuestions`, keep `recommendation` and `evidence`
   strings short, and add an assertion that the serialized result stays well under
   `SHTTP_RESP_MAX`.

6. **Open question — the brief `type: ["string","object"]` union may trip strict
   MCP clients.** Some MCP / JSON-Schema tool-call validators reject a union type on
   a single property. Since §2 already normalizes both forms server-side, consider
   either documenting two shapes or advertising `object` and accepting a bare string
   at parse time (normalized to `{focus:[s]}`) without exposing a union type in the
   schema, to maximize client compatibility. Not a blocker; folds into the §12 Q1
   naming/shape decision.

## Recommendation

Ship P1a + P1b + P1c: thread an optional `brief` into review mode with an open
mandate (as the empty-when-NULL middle `%s`, plumbed through both round callers),
retain the structured items as engine state (all severities returned,
blocking-only convergence), answer structured `questions`, and expose one
`CAP_DELEGATE`-gated MCP tool that targets review mode **through the async run
bridge** so it does not block the MCP request or lose cancellation. It reuses the
roundtable engine, the V1 async run lifecycle, the review contract, and the
existing config and capability model wholesale; the net new surface is a
prompt-builder seam, a result array, the async submit bridge, and one tool.
Validate it against a real change with a concrete invariant and a concrete
hypothesis to steer at (the Wolf shutdown-race / gst-wayland-display work is a good
first target: there is an invariant to protect and a race to ask about).

## §15 Fourth-pass findings (merge-result verification)

This pass reviewed the effective merge result against current `origin/testing`
(`d3eb402d`), not only the PR head, because the PR branch is older than the target
branch and the roundtable implementation comes from `testing`. The PR merges
cleanly, but these additional implementation details should be folded into the
phase plan.

1. **`/v1/runs/{id}` does not expose `in_progress` for op-run jobs today.** §14.A
   correctly says the queued snapshot starts as `status:"queued"` and the worker
   calls `openai_runs_store_set_status(..., OPENAI_RUN_IN_PROGRESS)`
   (`server_http_routes.inc:341`). But `openai_runs_store_set_status` updates only
   the store's internal enum (`openai_runs_store.c:152-162`); it does **not** call
   `openai_runs_store_update_json`, and `GET /v1/runs/{id}` returns the stored JSON
   snapshot verbatim (`server_http.c:827`, `openai_runs_store.c:279-290`). So a
   polling caller can see `queued` until the run finalizes, even while the internal
   status is `OPENAI_RUN_IN_PROGRESS`. If `ensemble_review` reuses the op-run store,
   either update the snapshot when the worker transitions to in-progress or
   document/pin the observable vocabulary as `queued -> completed|failed|cancelled`
   for this route. Add a route-level test that polls after `set_status` and proves
   the JSON status the client actually sees.

2. **The public V1/OpenAPI surface must be updated with the new request and result
   fields, not only the engine and MCP tool.** P1a adds `brief` to
   `POST /v1/delegate/roundtable`, and P1b adds structured result arrays. The
   current OpenAPI row (`api/openapi-server-v1.yaml:1676-1690`) lists only
   `prompt`, `mode`, `turns`, `rounds`, and `apply`, and the CLI/docs surfaces
   (`MANUAL.md`, `docs/COMMANDS.md`, generated config/API docs) describe the old
   result shape. P1a/P1b should explicitly include OpenAPI + generated-doc updates
   and run the server conformance checks, otherwise remote/thin clients will not
   learn that `brief`, `items`, `answeredQuestions`, and `coverageGaps` exist.

3. **The CLI `roundtable` marshaller needs a concrete `--brief` contract if P2
   lands.** The current marshaller accepts `--mode`, `--turns`, `--rounds`, and
   `--apply` (`src/cli_rpc_routes.inc:1687-1705`), but there is no stdin/file
   convention for a multi-line structured brief. If the optional CLI helper lands,
   define the input shape up front: either `--brief <text>`, `--brief-json
   <json>`, and/or `--brief-file <path>`, with clear precedence and the same 4 KB
   normalization/cap as HTTP/MCP. This avoids ad hoc shell-quoting behavior for
   the exact field agents and humans will use most.

4. **Returned review items should preserve `recommendation`, not only
   severity/category/location/summary.** §5 says the result item carries
   `severity`, `category`, `location`, `summary`, and the engine identity key, but
   the reviewer prompt already asks for `recommendation` and the agent-facing goal
   is actionable feedback. Dropping `recommendation` turns a machine-readable
   finding back into a diagnosis without the proposed fix. Include bounded
   `recommendation` text in the returned item, dedup/merge it separately from the
   blocking saturation key, and keep it under the response-size budget from
   §14.B.5.

5. **Deduped items should carry contributor/count metadata.** Once P1b dedups
   participant items for the final round, the caller loses whether one reviewer or
   the whole panel raised the issue. Add `sources` (participant names or stable
   indexes) and/or `count` to each returned item. This is cheap to collect at the
   parse-time capture point, helps agents prioritize fixes, and makes the
   structured result explainable without reading the consolidated prose.

6. **OpenAPI/result tests should check the actual async envelope.** The handler
   returns a normal `jo_ok()` body, but the async run stores it under
   `result` in an `op.run` snapshot. Tests for P1b/P1c should assert both shapes:
   the direct handler body contains the new item/question arrays, and
   `GET /v1/runs/{id}` returns them nested under `result` after finalization. This
   catches accidental tests that validate only the synchronous handler path while
   the MCP tool's poll path still drops or wraps the structured fields
   incorrectly.
