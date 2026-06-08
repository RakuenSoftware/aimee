# Proposal: Agent roundtable — round-robin collaborative drafting and review

- **State:** draft — pending review
- **Author:** JBailes
- **Date:** 2026-06-08
- **Charter roles:** Draft (initial + revision turns), Review (per-round
  critique + final synthesis/aggregation), Reason (convergence judging).
- **Scope:** `src/server/delegate_ensemble.c` (the engine being generalized —
  fix the participant-routing bug, add a multi-round loop alongside the existing
  single-shot path, replace the fixed result buffer with a bounded heap artifact,
  and fix the per-call `srand` reseed),
  `src/headers/delegate_ensemble.h` (new `delegate_roundtable_*` surface, heap
  result), `src/headers/agent_types.h` + `src/server/agent_runtime.c` (and the
  cross-platform mirror sources `src/posix/agent_runtime.c`,
  `src/posix/cmd_agent_delegate.c`, `src/windows/cmd_agent_delegate.c`, which must
  stay in lockstep under the thin-client split) — thread a
  per-task participant selector through `agent_task_t` → `parallel_worker` and a
  shared named-agent execution helper so both parallel fan-out and sequential
  turns route to distinct configured agents; clone the selected `agent_t` before
  mutation to close a pre-existing parallel-fan-out race (§0.1 step 4); and add an
  internal `agent_run_ex(…, double temperature, …)` seam that `agent_run` wraps
  with the current `0.3` default (so the ~28 existing `agent_run` call sites are
  unchanged) and that `parallel_worker` calls with `task->temperature`, making the
  currently-dead `agent_task_t.temperature` a live per-task control — see §0.1),
  the entry-point wiring that does not exist today (§0.2):
  first-class `/v1/delegate/*` endpoints (`src/server/server_http_routes.inc`,
  `api/openapi-server-v1.yaml`, native route handlers, and thin-client V1 call
  sites for `aimee delegate aggregate` / `roundtable`), backed by native
  `op == NULL` handlers with explicit `CAP_DELEGATE` and async `/v1/runs/{id}`
  finalization; never new `delegate.*` dispatch methods and never the retired
  `POST /v1/rpc` fallback;
  optionally `src/server/server_mcp_delegate.c` (MCP tools) backed by
  the same server-side implementation;
  `src/cmd_agent_delegate.c` is the lint-only body template for the new
  `roundtable` subcommand beside the existing (also lint-only) `aggregate` block,
  config plumbing
  (`src/headers/config.h`, `src/config.c`, `src/config_sections.c` for the inline
  `ensemble.*` plus a top-level `roundtable.*` parse, `src/config_save.c` for
  round-trip, and `src/config_fields.c` only for the new flat scalar
  `roundtable.*` keys so they are `aimee config get/set`-able — see §6), the
  existing parallel primitive in `src/headers/agent_exec.h`
  (`agent_run_parallel`) plus a new named-agent sequential wrapper around the same
  guarded execution path, the per-task provider/model resolution
  precedent in `src/server/aux_router.c` (reused as the routing seam), unit tests
  (`src/tests/test_delegate_ensemble.c` sibling, plus an un-stubbed routing test),
  docs (`MANUAL.md`). No new long-lived service, no new RPC route or transport,
  no new provider integration.

## Goal

Let **several agents collaborate on one artifact over multiple turns** — taking
turns to draft, revise, and critique a proposal (or any document) while each one
can see what the others produced in the previous round. The artifact converges
through iteration instead of being assembled in a single shot.

Aimee already has the *single-shot* version of this: **Mixture-of-Agents**
(`aimee delegate aggregate "<prompt>"` → `delegate_ensemble_run`,
`src/server/delegate_ensemble.c`). MoA fans a prompt to N tasks **in parallel**,
then runs **one** synthesis pass that reconciles their answers into a final
response. It is the right primitive, but it is both intentionally flat **and
currently broken in a way that defeats its headline value** (§0.1): the "N
diverse reference models" are not actually distinct models. Beyond that bug, the
shape is flat:

- **One round only.** Candidates never see each other. There is no turn-taking,
  no "agent B builds on agent A," and no chance to revise after reading a peer.
- **Aggregation, not collaboration.** The synthesis prompt
  (`build_synthesis_prompt`, `delegate_ensemble.c:72`) hands the aggregator a
  list of independent candidates and asks for "a single reconciled synthesis."
  Good for "answer this question well"; wrong for "co-author this document."
- **No convergence signal.** It always does exactly one fan-out + one merge.
  There is no notion of "keep going until the draft stops changing" or
  "stop when reviewers stop raising blocking issues."

This proposal **generalizes the ensemble engine into a roundtable**: the same
fan-out / cost-accounting / degrade-to-best machinery, wrapped in a bounded loop
where each round's shared artifact feeds the next round's prompts. It reuses the
existing orchestration shape, but before any of that is meaningful it must repair
the shipped engine on two levels: **(§0.2) wire an entry point** — the
marked-Done MoA feature has no caller in any shipped binary, so `aimee delegate
aggregate` does not run the ensemble at all — and **(§0.1) fix the participant
routing** — even once reachable, configured `ensemble.reference_models` are only
labels in the synthesis prompt, so every "participant" is the same default agent.
Both are step one (P0).

## §0 What already exists (so we don't rebuild it)

The hard parts — parallel execution, cost accounting, graceful degradation,
config plumbing — are done. The one part that is **not** done, despite the
feature being marked Done, is the entry point: nothing in any shipped binary
calls `delegate_ensemble_run` (see §0.2). What is confirmed in the tree:

- **The MoA engine.** `delegate_ensemble_run`
  (`src/server/delegate_ensemble.c:138`) already does: build N tasks → run them
  with `agent_run_parallel` (`agent_exec.h:41`) → estimate cost
  (`estimate_cost`, `ENSEMBLE_COST_PER_TOKEN`) → enforce a hard cost cap
  (`cfg->ensemble_max_cost_usd`) → require a minimum number of successes
  (`ensemble_min_successful`, default 2) or **degrade to the single best
  candidate** (`best_candidate`, by length) → shuffle candidate order to kill
  position bias (`shuffle_indices`, `delegate_ensemble.c:21`) → run a synthesis
  pass (`run_aggregator`, configurable aggregator). **Every one
  of these behaviors is exactly what a roundtable also needs per round.**
- **Config is already wired.** `ensemble_enabled`, `ensemble_reference_models[8]`,
  `ensemble_reference_count`, `ensemble_aggregator`, `ensemble_min_successful`,
  `ensemble_max_cost_usd` (`src/headers/config.h:1201-1206`), parsed by
  `config_parse_ensemble_section` (`src/config_sections.c:1068`). The roundtable
  reuses the configured participant list and aggregator. The configured reference
  list is **not** executable today (the `role = NULL` bug in §0.1); P0 fixes that
  by threading a per-task `agent` selector and routing each reference to a distinct
  agent. **Note:** these fields are parsed by a dedicated inline parser, **not**
  registered in `config_fields.c` (see `config_save.c:29`), so the "five-file
  pattern" does not apply to them — §6 specifies exactly where the new keys land.
- **A CLI seam exists — but only as lint-only code, not a live path.**
  `aimee delegate aggregate "<prompt>"` (`src/cmd_agent_delegate.c:504-535`)
  loads config, checks `ensemble_enabled`, loads `agent_config_t`, calls the
  engine, and prints the result with degrade/cost warnings. This is the right
  *shape* for a `roundtable` near-clone — **but `cmd_agent_delegate.c` is not
  linked into any shipped binary** (it sits in `CMD_SRCS` → `CMD_OBJS`, which
  appears only in `ALL_OBJS`, the lint/unit-test object set; `src/Makefile:433`).
  So this seam is a *template*, not a working entry point. See §0.2 for the full
  trace and the wiring that P0 must add before any of this is reachable.
- **Both execution shapes are available with one routing helper added.**
  `agent_run_parallel` (`agent_exec.h:41`) already covers "all participants act
  this round simultaneously." `agent_run_with_tools_write_enforce`
  (`agent_exec.h:37`, already used by `run_aggregator`) proves the single-turn
  sequential execution shape, but it routes by role; P0 adds the named-agent
  wrapper needed for a true round-robin (A → B → C, each seeing the prior). A
  *parallel-per-round* roundtable is `agent_run_parallel` per round. We support
  both (see §2).
- **An iteration template already exists.** The self-correcting delegate loop
  (`agent_loop_t` / `agent_loop_run`, `agent_exec.h:118-157`) is the existing
  precedent for "run, self-assess completion 0–100, feed prior context forward,
  stop at threshold or iteration cap." The roundtable borrows its **stop
  conditions** (`max_iterations`, `completion_threshold`,
  `agent_loop_parse_completion`) rather than inventing new ones.
- **Real charter roles exist** for the turns: `draft`
  (`src/role_templates.c:161`) for authoring turns and `review`
  (`role_templates.c:21`) for critique — the latter is already the ensemble's
  default aggregator role (`delegate_ensemble.c:113`).

So the net new code is: **the entry-point wiring (§0.2), the routing fix (§0.1),
a bounded loop around the existing fan-out, a bounded heap artifact, a
shared-artifact prompt builder, and a convergence check.**

## §0.1 First: the shipped ensemble does not use distinct models (bug, fixed here)

This is not a forward-looking gap — it is a correctness defect in the **already
merged, marked-Done** MoA feature, and this proposal fixes it as step one rather
than building on top of it.

`delegate_ensemble_run` builds every fan-out task identically with `role = NULL`
(`delegate_ensemble.c:156-162`):

```c
for (int i = 0; i < ref_count; i++) {
   tasks[i].role = NULL;          /* identical for every task */
   tasks[i].user_prompt = prompt;
}
agent_run_parallel(acfg, tasks, ref_count, results);   /* :167 */
```

`agent_task_t` (`agent_types.h:233-240`) carries **only** `role` to influence
routing — there is no per-task agent/model/provider field. With `role == NULL`,
`agent_run` (`agent_runtime.c:142`) skips the entire fallback loop
(`agent_has_role(ag, NULL)` never matches, `:154`) and falls through to
`agent_route(cfg, NULL)` (`:198`) — the **single default agent**, deterministically,
for all N tasks. `ensemble_reference_models` is consumed only as display labels in
`build_synthesis_prompt` (`:216-217`). **Net effect: today's "ensemble of diverse
reference models" is one agent answering the same prompt N times** — the only
variation is sampling noise.

**There is not even temperature diversity (a second, deeper defect).** A natural
assumption is that the fan-out at least varies sampling temperature per task. It
does not, and cannot today: `agent_run` (`agent_exec.h:27`,
`agent_runtime.c:131`) has **no temperature parameter** at all, and
`parallel_worker` (`agent_runtime.c:386-391`) calls it without passing
`task->temperature`. The call path hard-codes `0.3` (`agent_runtime.c:171/173`).
So `agent_task_t.temperature` is a **dead field on the parallel execution path** —
every parallel task runs the same agent at the same fixed temperature. This also
means the sibling vote fan-out in `agent_coord.c:497-501`, which sets
`tasks[i].temperature = 0.3 + 0.1*i` ("slight variation"), is a **no-op**:
`agent_vote()` has the identical illusion-of-diversity bug. Net: today's ensemble
has **zero** diversity — not by role, not by temperature — so the synthesis pass
reconciles N near-duplicate answers. P0 therefore also plumbs temperature through
the parallel path (see fix step 7), which repairs `agent_vote` at the same time.

**The fix (committed in P0, not deferred):**

1. Add an optional `const char *agent;` selector to `agent_task_t`
   (`agent_types.h`). When set, it names the configured agent/model that task must
   run on; when `NULL`, behavior is byte-identical to today (default route).
2. Add a named-agent execution helper used by both `parallel_worker`
   (`agent_runtime.c:386-391`) and sequential roundtable turns. When
   `task->agent` is set, resolve it the way `aux_router.c:51-58` already resolves
   a per-task `provider`/`model` (that file is the existing precedent for "this
   unit of work runs on a specific named provider/model"), bypassing role-based
   selection while keeping the normal guarded execution, metrics, hints, and
   provider-health accounting.
3. Define the selector contract explicitly: the named agent must exist, be
   enabled, be available for routing/credentials, and support the requested role
   or execution shape. A missing, disabled, unhealthy, or incompatible participant
   is a failed participant for `min_successful` purposes; it does **not** silently
   fall back to a different named agent, because that would collapse a diverse
   panel back into duplicate participants. Retry/same-tier fallback remains
   available only for the aggregator/judge paths unless a later proposal adds an
   explicit per-participant fallback list.
4. Clone the selected `agent_t` into local call state before applying runtime
   config, `ablation`, or `write_enforce`. This closes a **pre-existing data
   race**, not just a forward one: `agent_run` mutates fields on the selected
   `agent_t` in place (`agent_runtime.c:165-167` and `:203-205` —
   `agent_apply_runtime_config(ag); ag->ablation = …; ag->write_enforce = …`), and
   those `agent_t` structs are owned by the shared `agent_config_t`. Under
   `agent_run_parallel` every worker thread shares one `cfg`, so **today** — where
   all fan-out tasks route to the *same* default agent (the `role = NULL` bug
   above) — N threads already write the same `agent_t` fields concurrently;
   `agent_vote` (`agent_coord.c`) has the identical race. It is practically benign
   only because the colliding writes happen to store the same values, but it is
   undefined behavior and a TSan finding. Clone **unconditionally** in the
   named-agent helper / `parallel_worker` (not only when `task->agent` is set), so
   the race is closed for the existing same-agent fan-out too, and add a
   concurrency test that runs N parallel tasks against one agent under TSan.
5. `delegate_ensemble_run` sets `tasks[i].agent = cfg->ensemble_reference_models[i]`
   so the N references become N real participants.
6. **Un-stubbed test.** `test_delegate_ensemble.c` currently stubs
   `agent_run_parallel` wholesale, which is exactly why this bug shipped unseen.
   P0 adds a routing test that exercises the real selector resolution (three
   configured references → three distinct `agent_name` results) instead of
   stubbing it away, plus negative cases for missing, disabled, unhealthy,
   duplicate, and role-incompatible participants.
7. **Plumb temperature through the parallel path (repairs `agent_vote` too).**
   `agent_run` takes no `task` argument and has ~28 call sites across 17 files, so
   do **not** widen its signature directly. Add an internal
   `agent_run_ex(cfg, role, system_prompt, user_prompt, max_tokens, temperature,
   out)` that carries the hard-coded `0.3` onto a real parameter
   (`agent_runtime.c:171/173/227/229/333`), and keep `agent_run` as a thin wrapper
   that passes `0.3` — so every existing caller is byte-unchanged. Have
   `parallel_worker` call `agent_run_ex` with `task->temperature`, defaulting to
   `0.3` when the field is `0`/unset. This turns the already-present
   but dead `agent_task_t.temperature` into a real per-task sampling control,
   which (a) gives the ensemble a cheap orthogonal diversity axis even before
   distinct agents are configured and (b) makes `agent_coord.c:501`'s existing
   `0.3 + 0.1*i` perturbation actually take effect, fixing `agent_vote()`'s
   matching illusion-of-diversity bug. A unit test asserts two tasks with
   different `temperature` reach the request builder with different values.

**Routing fix changes the aggregate output — by design.** Note carefully: once
fix step 5 sets `tasks[i].agent = ensemble_reference_models[i]`, the aggregate
path **no longer produces the same text** it does today — it now fans out to N
distinct agents instead of one agent N times. That is the whole point. So the
"parity" guarantee for the existing public path is **engine-mechanics parity**
(`success` / `degraded` / `cost_capped` / `cost_usd` accounting, buffer/size
limits, aggregation max-tokens, the default-route path when `task->agent ==
NULL`), **not** response-text parity. The default-route path *is* byte-identical
when `tasks[i].agent == NULL`; the configured-reference path is intentionally
different and better. (See §4 — the golden-parity tests verify mechanics, not
text.) "Byte-identical *engine* behavior" never meant "still works for users":
per §0.2 the aggregate command has no reachable caller today regardless of this
fix.

## §0.2 Worse than mis-routed: the feature is unreachable (wire it first)

§0.1 is a correctness bug *inside* the engine. This is a layer below it: even
with §0.1 fixed, **no shipped binary ever calls `delegate_ensemble_run`**, so
`aimee delegate aggregate` cannot run the ensemble at all. The engine
(`src/server/delegate_ensemble.c`) *is* compiled into `aimee-server`
(`SERVER_SRCS`, `src/Makefile:376`), but it sits there as a defined-but-uncalled
symbol. Its only caller, `cmd_agent_delegate.c`, is lint-only (§0). A
tree-wide search confirms `delegate_ensemble` is referenced by exactly four
files: the engine, its header, its unit test, and the lint-only
`cmd_agent_delegate.c`.

**What `aimee delegate aggregate "<prompt>"` actually does today.** Traced end
to end:

1. The thin client does not recognize `aggregate` as a delegate subcommand —
   `delegate_arg_is_subcommand` (`src/cli_main.c:485-490`) lists only
   `plan / launch / status / log / history / --list-roles`. So the call falls
   through to the generic RPC dispatch.
2. `cli_rpc_lookup("delegate", …)` matches the catch-all delegate route
   `{"delegate", NULL, "delegate", …}` (`src/cli_rpc_routes.inc:243`).
3. `marshal_delegate` (`src/cli_rpc_routes.inc:1665`) maps **positional[0] →
   `role`** and **positional[1] → `prompt`**. So `"aggregate"` is sent as the
   *role* and the prompt becomes the request prompt:
   `{method:"delegate", role:"aggregate", prompt:"<prompt>"}`.
4. Server `handle_delegate` (`src/server/server_compute.c:1583`) canonicalizes
   the role — `delegate_role_canonicalize("aggregate")` returns `"aggregate"`
   unchanged (no alias, `src/server/delegate_role.c:40`). It then enforces a
   minimum prompt length: `if (strlen(prompt) < 20)` → **error "prompt too
   short."** For a longer prompt it runs **one ordinary delegate**; role
   `"aggregate"` matches no agent's role set, so `agent_route` falls back to the
   **single default agent** (`src/server/agent_runtime.c:198`).

Net user-visible behavior: a short prompt errors out; a normal-length prompt
silently runs a **single default-agent delegate** with a meaningless role label.
Either way there is no fan-out, no diverse references, no synthesis, no cost cap,
no degrade-to-best — none of the ensemble semantics — and **nothing tells the
operator the ensemble did not run.** The "Done" MoA feature is dead on arrival.

This is why the §0.1 routing test never caught the §0.1 bug *and* never caught
this: `test_delegate_ensemble.c` calls `delegate_ensemble_run` directly with a
stubbed `agent_run_parallel`, exercising a code path no shipped binary reaches.

**The missing wiring (P0 must add this before its value lands).** Add a
first-class `/v1/delegate/*` surface backed by **native V1 handlers only**. The
repo has retired `POST /v1/rpc`, keeps `v1-method-coverage-check` at zero
exclusions, and this feature must not add new `delegate.*` dispatch methods as a
shortcut. That means P0 must budget the native async seam explicitly:
`rh_dispatch_op_async` is not reusable as-is because it injects a method and
calls `loopback_rpc`, so aggregate/roundtable need a native enqueue/finalize
helper that drives `/v1/runs/{id}` without `server_dispatch`.

> **V1-only invariant.** The public entry point is first-class
> `/v1/delegate/aggregate` and `/v1/delegate/roundtable` with OpenAPI coverage,
> explicit `CAP_DELEGATE`, native `op == NULL` handlers, and async completion via
> `/v1/runs/{id}`. Do **not** add `delegate.aggregate` / `delegate.roundtable`
> dispatch methods, `cli_rpc_routes.inc` rows, server dispatch arms, coverage
> exclusions, or any `/v1/rpc` fallback.

- **V1 routes.**
  - Add `POST /v1/delegate/aggregate` and, once the loop exists,
    `POST /v1/delegate/roundtable` to `server_http_routes.inc`, plus matching
    paths in `api/openapi-server-v1.yaml`.
  - Leave each row's `op == NULL` and call the server-side implementation
    directly. Because `op == NULL` rows do **not** inherit
    `server_capability_for_method` (`server_http_routes.inc:1073`), the row must set
    `caps = CAP_DELEGATE` explicitly — a `caps = 0` initializer would make expensive
    LLM fan-out public and is a release blocker. Because `rh_dispatch_op_async` is
    not reusable as-is (it injects a method + calls `loopback_rpc`), P0 must add a
    native enqueue/finalize helper that drives `/v1/runs/{id}` without
    `server_dispatch`.
  - Add route-cap tests asserting both endpoints require `CAP_DELEGATE` and that
    TCP access stays denied unless the bearer/tier is allowed for delegate
    execution (`remote_writes=full`, as the existing delegate routes).
    aggregate/roundtable are LLM-heavy and roundtable runs many calls, so they
    must return a queued run handle and finalize status/result through the
    existing `/v1/runs/{id}` store; the buffered listener must **never** run the
    ensemble inline. The run moves queued → in_progress →
    completed/failed/cancelled and stores the final aggregate/roundtable JSON.
- **Server implementation.**
  - Add a server-linked implementation TU such as
    `src/server/delegate_ensemble_v1.c` (registered in `SERVER_SRCS`), or place
    the handlers beside the other native V1 handlers if that fits the local
    organization better.
  - The handler body is the `cmd_agent_delegate.c:504-535` block rewritten as
    native V1 work: read `prompt`/opts from the JSON body, load config +
    `agent_config_t` server-side, gate on `ensemble_enabled` (and
    `roundtable_enabled` if added), call the native aggregate/roundtable engine,
    and store/return `artifact` or `response` plus `degraded` / `cost_capped` /
    `cost_usd` / `rounds_run` / `converged` in the run result. If P0 fixes
    truncation for the new V1 path, it must expose a heap-result aggregate helper
    now; simply calling today's `delegate_ensemble_run` preserves the fixed
    `response[8192]` truncation and does not satisfy the P0 truncation row in
    §0.3. If heap output is deferred to P1, move that defect out of P0 rather than
    claiming it is fixed.
  - Apply the same `persona`/prompt-length guards intentionally (the ensemble
    prompt is the *task*, so the `<20` guard is appropriate; just make the error
    name the ensemble, not a generic delegate).
  - The worker must check `/v1/runs/{id}/stop` cancellation via
    `openai_runs_store_cancel_requested()` before starting each expensive stage
    (preflight, participant fan-out/sequential turn, aggregation, judge/scorer)
    and finalize as cancelled when requested. Tests should prove the V1 listener
    returns the queued run immediately and is not blocked by the ensemble worker.
- **Thin client.**
  - Add `aggregate` (and later `roundtable`) to `delegate_arg_is_subcommand`
    (`cli_main.c:485`) so usage/help and subcommand detection work.
  - Add a small CLI-side V1 request builder for these subcommands that puts
    **positional[0] -> `prompt`** (not `role`, the bug above) and folds flags
    (`--mode`, `--turns`, `--rounds`, `--apply`) into the JSON body.
  - POST to `/v1/delegate/aggregate` or `/v1/delegate/roundtable`, then poll
    `/v1/runs/{id}` until terminal and print the stored result. This is a native
    V1 call path, not a `cli_rpc_routes.inc` method mapping.
  - This bespoke client path will **not** be generated by
    `scripts/gen-cli-v1-routes.py`, because native handlers with `op == NULL` are
    intentionally excluded from the generated method map. Add a CLI test that
    fails if `aimee delegate aggregate "<prompt>"` falls through to
    `cli_rpc_lookup("delegate", ...)`, sends `"aggregate"` as `role`, or touches
    `cli_rpc_routes.inc`.
- **MCP tool (parallel, optional in P0).** Expose `aggregate` / `roundtable`
  MCP tools in `server_mcp_delegate.c`, but have them call the same server-side
  helper the native V1 route uses, not a third implementation.
- **Build + de-dup.** Register any new TU in `src/Makefile` (`SERVER_SRCS`) and,
  if applicable, the cmake profile; retire the lint-only `aggregate` block in
  `cmd_agent_delegate.c` (or keep it strictly as a platform CLI helper that
  builds the V1 request) so there is one live implementation, not two divergent
  ones.
  Add a negative route-surface check for P0: no new `delegate.aggregate` /
  `delegate.roundtable` string may appear in `src/server/server.c`,
  `src/cli_rpc_routes.inc`, server dispatch methods, coverage exclusions, or any
  `/v1/rpc` fallback. The public path is the first-class `/v1/delegate/*` routes
  plus optional MCP tools backed by the same server-side helper, never dispatch
  methods and never `/v1/rpc`.

Until this wiring exists, the §0.1 routing fix repairs a path with no callers.
P0 therefore **wires the entry point and fixes the routing in the same phase**
(§7), with an end-to-end reachability test (CLI/V1 → engine → run result) that
would have caught both this gap and the §0.1 bug.

## §0.3 Standalone fix: P0 ships on its own, ahead of the roundtable

P0 is **not** scaffolding for the roundtable — it is a self-contained repair of a
shipped, marked-Done feature that does not work, and it should be reviewable and
mergeable **without committing to any of §1–§9**. The roundtable (P1+) builds on
it, but P0 stands alone and delivers value the day it lands: `aimee delegate
aggregate` runs a real Mixture-of-Agents ensemble for the first time. Concretely,
the standalone P0 changeset is:

| Defect (all verified in-tree) | Fix | Files |
|---|---|---|
| Engine unreachable — no shipped binary calls `delegate_ensemble_run` (§0.2) | `POST /v1/delegate/aggregate` native `op == NULL` handler (+ explicit `CAP_DELEGATE`) finalizing via `/v1/runs/{id}` + thin-client `aggregate` subcommand; never dispatch methods and never `/v1/rpc` | `server_http_routes.inc`, `api/openapi-server-v1.yaml`, `server/*`, `cli_main.c`, `Makefile` |
| All N references route to the one default agent (`role = NULL`, §0.1) | Add `const char *agent;` to `agent_task_t`; named-agent execution helper resolving like `aux_router.c:51-58`; clone `agent_t` before mutation; set `tasks[i].agent = ensemble_reference_models[i]` | `agent_types.h`, `agent_runtime.c`, `delegate_ensemble.c` |
| `agent_task_t.temperature` is dead on the parallel path → zero diversity, also breaks `agent_vote` (§0.1 step 7) | Add internal `agent_run_ex(…, temperature, …)`; `agent_run` wraps it with `0.3`; `parallel_worker` passes `task->temperature` | `agent_exec.h`, `agent_runtime.c` (+ `posix/agent_runtime.c`) |
| Pre-existing data race: `agent_run` mutates shared `cfg`-owned `agent_t` (`:165-167`, `:203-205`) under `agent_run_parallel`; today all tasks hit one default agent so N threads write one struct (also `agent_vote`) (§0.1 step 4) | Clone `agent_t` into local call state **unconditionally** before mutation; add a TSan concurrency test | `agent_runtime.c` |
| `srand(time(NULL))` per call clobbers global RNG, collides within a second (§4) | Seed once at process start or thread `rand_r` state through `shuffle_indices` | `delegate_ensemble.c` |
| Truncation: result copies silently `snprintf` into fixed `response[8192]` (`:181/200/233`), while a too-large synthesis prompt instead hard-fails the **whole run** (`build_synthesis_prompt` → `-1`, `:218`) — both wrong for a growing multi-round draft (§3) | Heap-grown result + budget-bounded prompt on the new path; keep fixed buffers only on the frozen legacy path until parity is proven | `delegate_ensemble.h`, `delegate_ensemble.c` |
| Bug shipped unseen because the test stubs `agent_run_parallel` (§0.1 step 6) | Un-stubbed routing test + an end-to-end reachability test (CLI/V1 → engine → run result) | `tests/test_delegate_ensemble.c`, `tests/Rules.mk` |

Everything from §1 onward (multi-round loop, draft/review modes, sequential
turns, convergence judging, keep-best, config keys) is **strictly additive on top
of a working P0** and can be staged or dropped without reopening the P0 fix. A
reviewer who only wants the bug fixed can approve P0 and stop there.

## §0.4 Process gap: how a never-implemented feature was marked "Done"

Worth fixing alongside the code, because it is the reason this shipped: the MoA
entry in `docs/PROPOSALS.md` was marked **Done** while no binary ever called the
engine, and nothing flagged it. Two cheap guardrails would have caught it:

- **A reachability/coverage smell test.** A feature whose only non-test caller is
  a lint-only `cmd_*.c` (the legacy, unlinked CLI layer — see the project note
  that `cmd_*` files are lint-only under the thin-client split) is, by
  definition, unreachable. A CI check that flags exported "feature" entry points
  with no caller in any shipped-binary object set would have caught this before
  the "Done" label.
- **A proposal-index link check.** `PROPOSALS.md`'s MoA entry linked
  `proposals/done/mixture-of-agents-ensemble-delegate.md`, a file that does not
  exist (and 45 of its 47 links resolve to nothing — `done/` proposals are
  delisted as files by convention). That convention makes a genuinely-missing or
  mistyped link invisible. A check that (a) every `pending/` and `accepted/` link
  resolves and (b) every `done/` entry that *claims* a file actually has one would
  keep the index honest. This is out of scope for the code fix but is logged here
  as the root-cause control.

## §1 What a roundtable is

A roundtable runs up to `R` rounds over a shared artifact `D` (the draft).
Participants are the configured reference models after they have been resolved to
actual runnable agents. Two collaboration *modes*, selected per invocation:

**Mode A — collaborative drafting (`--mode draft`, default).** There is one
living artifact. Round 1: each participant produces an initial draft from the
task; the aggregator merges them into `D₁` (this is exactly today's MoA, and is
the natural round-1 behavior). Rounds 2..R: each participant receives the task +
the current `Dₙ₋₁` + a one-line note of what changed last round, and returns a
*revised full draft*. The aggregator reconciles the revisions into `Dₙ`. The loop
stops when the draft converges (§3) or `R` is hit. Output: the final `D`.

**Mode B — review / feedback (`--mode review`).** The artifact is *fixed input*
(the document under review, passed via the prompt or a file). Each round, each
participant returns **structured feedback** (blocking issues, suggestions, nits)
rather than a rewrite. The aggregator deduplicates and ranks the feedback into a
single review. Rounds let reviewers react to each other ("B disagrees with A's
blocking issue #2"). The loop stops when a round surfaces **no new blocking
issues**. Output: a consolidated review, optionally followed by one final `draft`
turn that applies it (`--apply`).

Review-mode feedback uses a structured contract so convergence is testable, not
just a free-form aggregator judgment. Each reviewer returns JSON items with at
least:

```json
{
  "items": [
    {
      "severity": "blocking|suggestion|nit",
      "category": "correctness|security|performance|maintainability|style",
      "stable_key": "optional model-supplied key — display only; the engine computes its own identity key (§1)",
      "location": "file:line or artifact section",
      "summary": "one-sentence issue"
    }
  ]
}
```

The engine treats `severity == "blocking"` as the stop-relevant class, but it does
**not** trust the reviewer-supplied `stable_key` for issue identity — an LLM will
not emit a key that is stable across rounds or consistent across reviewers, so
trusting it risks both premature saturation (a key coincidentally reused) and
never-saturating (the same issue re-keyed every round, so the loop always runs to
`max_rounds`). Instead the engine **computes** the identity key itself by
normalizing and hashing `category` + `location` (falling back to a shingled hash of
`summary` when `location` is absent); the model's `stable_key`, when present, is
recorded for display only. The aggregator may still semantically deduplicate the
final prose, but the saturation decision rests on an engine-computed,
model-independent key.

Both modes are the *same loop* — they differ only in the per-turn instruction
(revise vs. critique) and the convergence predicate (draft stabilized vs. no new
blockers). This keeps the engine single.

## §2 Turn discipline: round-robin vs. parallel-per-round

The phrase "round-robin" admits two honest implementations; we expose both
because they trade latency for cross-pollination:

- **`--turns parallel` (default).** Within a round all participants act at once
  via `agent_run_parallel`, each seeing the *previous* round's merged artifact.
  Cheapest wall-clock (one parallel batch per round), and it reuses the ensemble
  fan-out unchanged. Peers influence each other across rounds, not within one.
- **`--turns sequential` (true round-robin).** Within a round, participant `k`
  sees participant `k−1`'s output *from this same round*. Implemented as a loop
  of named-agent sequential calls using the same selector contract as P0; using
  `agent_run_with_tools_write_enforce` directly is insufficient because that API
  routes by role rather than by participant name. Maximum cross-pollination (later
  speakers react to earlier ones immediately) at the cost of serial latency. To
  avoid a fixed speaking order biasing the result, the participant order is
  **shuffled each round** with the existing `shuffle_indices`
  (`delegate_ensemble.c:21`).

Both share the cost cap, `min_successful` floor, and degrade-to-best fallback
per round.

## §3 Convergence and stop conditions

Borrowed from `agent_loop_t` (`agent_exec.h:122`), not reinvented:

- **Hard cap:** `roundtable_max_rounds` (default 3). Always terminates.
- **Wall-clock deadline:** `roundtable_deadline_ms` (default 600000 = 10 min, `0`
  = off). Sequential-turns mode is R×N serial LLM calls plus R×(aggregator+scorer)
  and can run many minutes; `agent_loop_t` already bounds its loop on time, and the
  roundtable should too. Checked at each round boundary and before each expensive
  stage (alongside the `/v1/runs/{id}/stop` cancellation check); on expiry the loop
  stops and returns the best artifact so far with `deadline_hit` set, never a
  partial mid-stage write.
- **Draft mode — stability:** convergence of *text* is mechanically measurable, so
  the primary signal is **deterministic** — compute a normalized change ratio
  between `Dₙ` and `Dₙ₋₁` (token-set Jaccard or normalized edit distance) and stop
  when it is below `roundtable_converge_threshold` (default 10, as a 0–100
  percentage). This avoids paying an LLM call — and its own noise — to judge
  something a diff measures exactly. The `reason`-role 0–100 judge is kept only as
  an **optional tiebreak** for the ambiguous band (a large reword that barely
  changes meaning), not as the gate. This differs deliberately from
  `agent_loop_parse_completion` (`agent_exec.h:144`), which judges *task completion*
  (genuinely an LLM call); text convergence does not need a model in the common
  path.
- **Review mode — saturation:** stop when a round returns no `severity ==
  "blocking"` item whose **engine-computed** identity key (normalized
  `category`+`location` hash, §1 — *not* the model's `stable_key`) was not already
  raised in a prior round. Semantic dedup stays in the aggregator for final
  presentation; the engine's stop predicate is deterministic, model-independent,
  and unit-testable.
- **Cost ceiling (shared, hard) — post-hoc hard stop, preflight once cost is real.** The existing
  `ensemble_max_cost_usd` cap is a *running* total across all rounds. Today's
  single-shot path only checks cost *after* the fan-out has already spent it
  (`delegate_ensemble.c:169-173`), so the cap reports but never prevents. Over R
  rounds that compounds: post-hoc-only enforcement can overshoot by a full round ×
  N participants before the loop notices. This proposal therefore checks **at the
  round boundary**: before dispatching round *n*, estimate that round's cost from
  `(participants × effective_per_task_max_tokens) + aggregator + scorer` and, once a
  realistic per-provider cost exists (see the sub-bullet), **do not start the
  round** if `accumulated + estimate` would cross the cap. Until that cost lands,
  the preflight estimate only **warns** and the **post-hoc** accounting below is the
  authoritative hard stop — so the cap is never a no-op even in warn mode, it just
  cannot overshoot by more than one round. `effective_per_task_max_tokens` is
  resolved from the task value when set, otherwise from the selected
  participant/provider default; a literal `0` cannot be treated as a zero-cost turn.
  Either way, cap handling returns the **best artifact so far** (§4, keep-best) with
  `cost_capped = 1`.
  - **The cost model must improve, or preflight over-blocks (review finding).**
    `ENSEMBLE_COST_PER_TOKEN` is a single flat `$15/MTok` constant
    (`delegate_ensemble.c:19`) that bears no relation to the actual delegates
    (minimax / mimo-2.5 / mistral). Worse, a *worst-case* preflight that multiplies
    by `effective_per_task_max_tokens` is pathological on these providers: their
    advertised `max_tokens` run to 131k–524k, so `participants × max_tokens ×
    flat-rate` will exceed any sane `ensemble_max_cost_usd` and **refuse to start
    rounds that would actually be cheap** (real completions are a small fraction of
    `max_tokens`). P0/P1 must therefore (a) source per-provider/per-token cost from
    the model-capability registry rather than one global constant, and (b) base the
    preflight estimate on a realistic expected completion size (e.g. a configurable
    `roundtable_preflight_tokens` budget or an EWMA of observed completion tokens),
    treating the raw `max_tokens` ceiling as an upper bound for logging only. Until
    a real per-provider cost exists, the preflight gate should default to *warn*,
    not *block*, so it cannot strand a valid run.
- **Size ceiling — bounded heap artifact, summarize-forward, no silent truncation.**
  Today the result is a fixed `char response[8192]` (`delegate_ensemble.h:12`),
  silently `snprintf`-truncated at the three copy sites (`:181/200/233`), while a
  too-large synthesis prompt takes the opposite-but-also-wrong path:
  `build_synthesis_prompt` returns `-1` and the **entire run fails** (`:218`) rather
  than degrading. Both are exactly the failure mode a growing multi-round draft
  hits. The roundtable does not inherit either: `roundtable_result_t`
  holds a **heap `char *artifact`** (grown to fit, freed by the caller, §4), and
  per-round prompts are assembled under an explicit byte budget derived from the
  participant context window. When `task + artifact + peer notes` would exceed the
  budget, `build_round_prompt` **summarizes the oldest peer notes forward via one
  aggregator pass** rather than truncating mid-token, and sets `degraded = 1` with a
  `truncated` flag in the result so the condition is observable, never silent.

## §4 Engine shape (generalize, don't fork)

Add to `delegate_ensemble.c`/`.h` a loop that *calls into* the existing
single-round primitives:

```c
typedef enum { ROUNDTABLE_DRAFT, ROUNDTABLE_REVIEW } roundtable_mode_t;
typedef enum { ROUNDTABLE_PARALLEL, ROUNDTABLE_SEQUENTIAL } roundtable_turns_t;

typedef struct {
   roundtable_mode_t mode;
   roundtable_turns_t turns;
   int max_rounds;          /* default 3 */
   int converge_threshold;  /* 0-100 normalized change-ratio stop, default 10 */
   int deadline_ms;         /* wall-clock cap, default 600000; 0 = off (§3) */
   int apply_review;        /* mode B: final draft turn applies the review */
} roundtable_opts_t;

typedef struct {
   char *artifact;          /* heap, grown to fit; caller frees. best round (§3) */
   int rounds_run;
   int converged;           /* 1 = stopped on convergence, 0 = hit cap/rounds */
   int degraded;            /* a round fell back to best-candidate, or summarized */
   int truncated;           /* a peer note was summarized forward to fit budget */
   int cost_capped;         /* running cost cap hit (preflight or post-hoc) */
   int deadline_hit;        /* wall-clock deadline expired (§3) */
   int best_round;          /* which round produced the returned artifact */
   double cost_usd;         /* accumulated across all rounds + aggregators */
} roundtable_result_t;

int delegate_roundtable_run(agent_config_t *acfg, const config_t *cfg,
                            const char *task, const roundtable_opts_t *opts,
                            roundtable_result_t *out);
```

`delegate_roundtable_run` is a loop whose body is *the existing ensemble round*:
build per-participant tasks (each task routed to a distinct configured participant
via the §0.1 `agent` selector, each `user_prompt` composed of task + current
artifact + peer notes via a budget-bounded `build_round_prompt`), fan out (parallel
or sequential per §2), apply cost/min-success/degrade with the same semantics as
today, then call `run_aggregator` to fold the round into the next artifact. After
each round, run the convergence check (§3) and break early.

**Keep-best-so-far is the default, not an option.** A pathological round can make
the draft *worse*, and the deterministic change ratio (§3) detects only *low
change*, not *regression*. So each round's aggregated artifact is scored for
**quality against the task** by one `reason`-role call (this is a genuine judgement,
unlike the mechanical convergence metric, so it stays an LLM call), the engine
retains the **highest-scored** artifact across rounds, and `out->artifact` /
`out->best_round` return that — never blindly the last round. This reuses the
existing `best_candidate` idea (`delegate_ensemble.c:197`) one level up.

**Fix the `srand` reseed while here.** `delegate_ensemble.c:212` calls
`srand((unsigned)time(NULL))` on *every* invocation; two calls in the same second
produce the same shuffle and it clobbers process-global RNG state. The roundtable
shuffles **per round** (§2), so this latent bug would silently disable the
position-bias control (identical orders every round within a second). The fix:
seed once at process start (or use a local `rand_r` state threaded through
`shuffle_indices`), and remove the per-call `srand`. The aggregate path inherits
the fix; golden parity holds because the shuffle distribution is unchanged.

Keep `delegate_ensemble_run` behavior frozen in the first implementation. Extract
shared helpers where useful, but do not replace the public aggregate path until
golden parity tests prove the **engine mechanics** — `success`, `degraded`,
`cost_capped`, response-size limit, aggregation max tokens, and `cost_usd`
semantics — for the existing one-shot cases. **Parity is on mechanics, not
response text:** the P0 routing fix (§0.1 step 5) deliberately changes the
aggregate output from "one default agent N times" to "N distinct configured
agents," so asserting identical synthesis text would contradict the fix. Text
parity is asserted only for the unchanged default-route path (`task->agent ==
NULL`). Once mechanics parity is proven, `aggregate` can be documented as the
one-round roundtable special case.

`build_synthesis_prompt` may be refactored into a parameterized prompt builder so
the "you are a synthesis aggregator" framing (draft merge) and a new "you are
consolidating peer reviews" framing (review dedup) share one code path. The
refactor must preserve the current aggregate prompt exactly until parity tests
permit changing the implementation.

## §5 CLI

A new subcommand built on the §0.2 V1 wiring (CLI JSON body builder +
`POST /v1/delegate/roundtable` + `/v1/runs/{id}` polling), using the
`cmd_agent_delegate.c:504` block only as a body template — **not** as the live
path, since that file is lint-only:

```
aimee delegate roundtable "<task or path to artifact>" \
    [--mode draft|review] [--turns parallel|sequential] \
    [--rounds N] [--apply]
```

- Reuses the same guard (`ensemble_enabled` → reuse, or a sibling
  `roundtable_enabled`; recommend gating on the existing `ensemble_enabled` so
  there is one "multi-agent is on" switch).
- Prints the final artifact; on `--mode review` prints the consolidated review;
  emits the same degrade/cost warnings the engine produces, plus `rounds_run` /
  `converged`, read from the V1 run result.
- `aimee delegate aggregate` becomes a **genuinely working** one-shot alias for
  the first time once §0.2 lands (it does not work today), and stays the
  documented `rounds==1` special case.

## §6 Config (reuse ensemble participants)

Add a top-level `roundtable` config object beside the existing top-level
`ensemble` object (near `config_parse_ensemble_section`,
`src/config_sections.c:1068`):

| key | default | meaning |
|-----|---------|---------|
| `roundtable.max_rounds` | 3 | hard round cap |
| `roundtable.converge_threshold` | 10 | normalized change-ratio stop (0–100) |
| `roundtable.deadline_ms` | 600000 | wall-clock cap (0 = off) |
| `roundtable.turns` | `parallel` | default turn discipline |

Participants (`ensemble.reference_models`), aggregator
(`ensemble.aggregator`), `min_successful`, and `max_cost_usd` are reused as the
configuration source, with the participant-routing fix from §0.1. The new
roundtable defaults are serialized as a **top-level** `roundtable` object:

```yaml
roundtable:
  max_rounds: 3
  converge_threshold: 10
  deadline_ms: 600000
  turns: parallel
```

**Placement (corrected).** The `ensemble.*` fields are *not* registered in
`config_fields.c`; they use a dedicated inline parser/saver pair
(`config_parse_ensemble_section` at `config_sections.c:1068`, inverse in
`config_save.c:62-81` — see the `config_save.c:29` comment naming the
"dogfood/ensemble/integrity/identity" inline parsers). So the often-quoted
"five-file pattern" does **not** describe these keys. The new `roundtable.*` keys
land as follows, and because they are flat scalars (unlike the
`reference_models` array) they *can* additionally be CLI-settable:

- `config.h` — four new fields on `config_t` (`roundtable_max_rounds`,
  `roundtable_converge_threshold`, `roundtable_deadline_ms`,
  `roundtable_turns[16]`).
- `config.c` — defaults (3, 10, 600000, `"parallel"`), beside the ensemble defaults
  at `config.c:507-509`.
- `config_sections.c` — add a sibling inline parser for the top-level
  `roundtable` object, near the existing ensemble parser.
- `config_save.c` — extend the inverse saver for round-trip of the top-level
  `roundtable` object.
- `config_fields.c` — register **only** the flat scalars (`max_rounds`,
  `converge_threshold`, `deadline_ms`, and the short `turns` string) so `aimee
  config get/set roundtable.max_rounds` works. (The array-valued `ensemble.*` keys
  stay inline-only, as today.)

This is a deliberate, stated decision rather than an inherited convention: scalars
are CLI-settable; the array participant list is not.

## §7 Phasing

1. **P0 — wire the entry point (§0.2) + fix the routing bug (§0.1) + the `srand`
   reseed.** These ship together because the routing fix is invisible without a
   reachable caller.
   - *Wiring (§0.2):* add `POST /v1/delegate/aggregate` (and the
     `/v1/delegate/roundtable` placeholder shape if useful) with OpenAPI coverage,
     explicit `CAP_DELEGATE`, async finalize through `/v1/runs/{id}`, and a CLI
     builder that posts `{prompt,...}` and polls the run. Use native
     `op == NULL` handlers plus the new enqueue/finalize seam; do not add
     `delegate.*` dispatch methods, coverage exclusions, `cli_rpc_routes.inc`
     mappings, or any `/v1/rpc` fallback. The buffered listener must never run the
     ensemble inline. Retire the lint-only
     `aggregate` block or keep it only as request-building helper code.
   - *Routing (§0.1):* thread an optional `agent` selector through
     `agent_task_t` and a shared named-agent execution helper, resolving named
     agents the way `aux_router.c:51-58` already does; point each ensemble
     fan-out task at `ensemble_reference_models[i]`. Thread `temperature` through
     `agent_run`/`parallel_worker` so the dead `agent_task_t.temperature` field
     becomes live (also repairs `agent_vote`, §0.1 step 7). Move `srand` out of
     the per-call path.
   - *Tests:* an **end-to-end reachability test** (CLI/V1 → native worker →
     engine → run result) that fails on today's tree — this is the test whose
     absence let both the unreachability and the §0.1 bug ship. Add route-cap
     tests (`CAP_DELEGATE`, TCP privilege denial), a non-blocking listener test, a
     cancellation test, and negative surface tests proving no
     `delegate.aggregate` / `delegate.roundtable` dispatch methods,
     `cli_rpc_routes.inc` rows, coverage exclusions, or `/v1/rpc` fallback were
     added. Plus **un-stubbed routing tests**: three configured
     references → three distinct `agent_name` results,
     missing/disabled/unhealthy participants, duplicate names, role
     incompatibility, a temperature test (two tasks with different
     `task->temperature` reach the request builder with different values), and a
     TSan concurrency test proving the unconditional `agent_t` clone closes the
     same-agent parallel-fan-out race (§0.1 step 4) as well as the new
     named-agent path. The existing stubbed ensemble tests also re-run green.

   This phase ships independently and **makes the marked-Done MoA feature
   actually work for the first time** (it does not today, §0.2); everything below
   builds on a reachable, real fan-out.
2. **P1 — engine generalization (`mode draft`, `turns parallel`).** Add
   `delegate_roundtable_run` looping the existing round; add budget-bounded
   `build_round_prompt` (summarize-forward, no silent truncation, §3); heap
   `char *artifact` with keep-best-so-far (§4); **preflight + post-hoc** running
   cost cap at loop level using effective max-token resolution (§3). Keep
   `delegate_ensemble_run` frozen while
   extracting shared helpers. Unit tests sibling to `test_delegate_ensemble.c`:
   convergence stop, preflight cap stop, post-hoc cap stop, degrade path,
   prompt-budget summarize-forward (assert `truncated` set, not silent),
   keep-best-selects-not-last, and aggregate golden parity for the existing public
   path.
3. **P2 — convergence + sequential turns.** Add the **deterministic** draft change
   ratio (token-set Jaccard / normalized edit distance, §3) as the primary stop
   signal, the optional `reason`-role tiebreak judge for the ambiguous band (a
   dedicated prompt emitting the `{"completion":N}` shape that
   `agent_loop_parse_completion` parses, `agent_exec.h:144` — the stock `reason`
   template alone does not emit that shape), the `reason`-role keep-best quality
   scorer (§4), the `roundtable_deadline_ms` wall-clock cap (§3), and `--turns
   sequential` (per-round shuffle via the fixed RNG). Tests: early-stop on a stable
   draft (no LLM call needed), keep-best on a regressing draft, deadline expiry
   returns best-so-far with `deadline_hit`, and order-shuffling.
4. **P3 — review mode.** `--mode review`, structured-feedback JSON prompt,
   blocking-issue saturation stop keyed on the **engine-computed** identity key
   (normalized `category`+`location` hash, §1 — never the model's `stable_key`),
   aggregator dedup, optional `--apply` final draft turn. Malformed reviewer JSON
   counts as a failed participant after one optional repair prompt and never
   silently satisfies convergence. Tests for saturation stop, dedup, malformed
   JSON, key stability across two rounds that re-describe the same issue in
   different words, and the `summary`-shingle fallback when `location` is absent.
5. **P4 — CLI + config + docs.** `aimee delegate roundtable`, the `roundtable.*`
   config keys (inline parse/save + scalar registration in `config_fields.c`, §6),
   `MANUAL.md` section. Live-validate against the configured delegates (minimax /
   mimo-2.5 / mistral) — now genuinely distinct after P0 — drafting and then
   reviewing a small real proposal end-to-end.

## §8 Risks / non-goals

- **Cost.** Per *draft* round the call count is **N participants + 1 aggregator +
  1 keep-best quality scorer = N + 2** LLM calls — the convergence check is now
  **deterministic** (§3) so it is not a call in the common path (an optional
  tiebreak judge adds at most one more, only in the ambiguous band). A round is
  still meaningfully more than "N participants × turn cost," and the cost estimate
  (§3) must include the aggregator + scorer overhead, not only the participant
  fan-out. Over R rounds total cost is roughly R × (N + 2) turns. Bounded by the
  running cost cap — the **post-hoc** check is the authoritative hard stop (it can
  overshoot by at most one round × (N+2)); the **preflight** gate tightens that to
  "refuse to start a round that would cross the cap" only once a realistic
  per-provider cost replaces the flat constant (§3), warning rather than blocking
  until then — plus a low default `max_rounds = 3`, the `roundtable_deadline_ms`
  wall-clock cap, and early convergence stops. The CLI prints accumulated
  `cost_usd` like `aggregate` does.
- **Routing ambiguity — resolved, not assumed away.** The shipped ensemble does
  not route fan-out tasks to the configured reference names at all (the §0.1 bug).
  P0 fixes this with a per-task `agent` selector, a named-agent execution helper,
  and un-stubbed selector tests, so the roundtable runs genuinely distinct peers
  rather than repeated calls to one default agent presented as a panel. Named
  participants fail closed rather than silently falling back to another peer.
- **Prompt growth — handled by design.** Per-round prompts are assembled under an
  explicit byte budget; over-budget peer notes are summarized forward (one
  aggregator pass) and flagged `truncated`, never silently `snprintf`-cut. The
  artifact is heap-grown rather than a fixed buffer (§3, §4).
- **Non-convergence / drift — kept-best by default.** A pathological round can make
  the draft worse, and the convergence judge detects only *low change*, not
  *regression*. The engine therefore scores each round and returns the
  **highest-scored** artifact (`best_round`), not the last (§4). This is default
  behavior, not an opt-in.
- **Entry point is first-class V1; no dispatch/RPC fallback.** The public surface
  is `POST /v1/delegate/*` with OpenAPI coverage, explicit `CAP_DELEGATE`, native
  `op == NULL` route handlers, and no new `delegate.*` dispatch methods or retired
  `POST /v1/rpc` bridge. No new transport/protocol, no long-lived service, no
  persistence, no new provider integration. Long-running aggregate/roundtable
  calls finalize through the
  existing `/v1/runs/{id}` contract and never block the buffered listener.
- **Not the self-correcting `agent_loop`.** That loop is *one* agent improving
  its own output; the roundtable is *many* agents collaborating. They share stop-
  condition machinery but are deliberately separate engines.

## §9 Why adapt the ensemble rather than build new

The ensemble already encodes the three things that are easy to get wrong in
multi-agent orchestration — **bounded cost, graceful degradation when providers
fail, and position-bias control** — and it is already config-wired (the
*runtime* path is not yet wired; §0.2). A greenfield "debate" engine would
re-derive the hard parts. Generalizing `delegate_ensemble.c` into a loop keeps
one engine, one config section, one CLI family, makes the (newly reachable)
`aggregate` a provable special case after parity tests, and confines the new
surface to the §0.2 entry-point wiring, the §0.1 routing fix, a loop,
budget-bounded prompt assembly, and a convergence check. As a bonus,
generalizing here forces the fix of five latent defects the shipped engine has
been quietly carrying — its complete unreachability (§0.2), the unrouted
references (§0.1), the dead per-task `temperature` on the parallel path (§0.1
step 7, which also silently breaks `agent_vote`), the shared-`agent_t` data race
under parallel fan-out (§0.1 step 4), and the per-call `srand` reseed (§4).
