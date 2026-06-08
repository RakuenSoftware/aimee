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
  result), `src/headers/agent_types.h` + `src/server/agent_runtime.c` (thread a
  per-task participant selector through `agent_task_t` → `parallel_worker` and a
  shared named-agent execution helper so both parallel fan-out and sequential
  turns route to distinct configured agents — see §0.1),
  the entry-point wiring that does not exist today (§0.2):
  `src/cli_main.c` + `src/cli_rpc_routes.inc` (recognize the subcommands, add
  routes + `marshal_delegate_aggregate`/`marshal_delegate_roundtable`),
  `src/server/server.c` (dispatch-table entries) and a new server-linked TU
  `src/server/delegate_ensemble_rpc.c` for `handle_delegate_aggregate` /
  `handle_delegate_roundtable` (registered in `src/Makefile` `SERVER_SRCS`),
  optionally `src/server/server_mcp_delegate.c` (MCP tools) and the `/v1`
  surface (`server_http_routes.inc`, `api/openapi-server-v1.yaml`,
  `cli_v1_routes_gen.inc`) or a documented method-coverage exclusion;
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
  docs (`MANUAL.md`). No new long-lived service, no new RPC transport, no new
  provider integration.

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
variation is sampling noise. (The sibling fan-out in `agent_coord.c:497-501` has
the same `role`-only limitation but at least perturbs `temperature` per task; the
ensemble does not even do that.)

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
   config, `ablation`, or `write_enforce`. Current `agent_run` mutates fields on
   the selected `agent_t`; doing that against shared `agent_config_t` entries from
   parallel fan-out would create races between participants.
5. `delegate_ensemble_run` sets `tasks[i].agent = cfg->ensemble_reference_models[i]`
   so the N references become N real participants.
6. **Un-stubbed test.** `test_delegate_ensemble.c` currently stubs
   `agent_run_parallel` wholesale, which is exactly why this bug shipped unseen.
   P0 adds a routing test that exercises the real selector resolution (three
   configured references → three distinct `agent_name` results) instead of
   stubbing it away, plus negative cases for missing, disabled, unhealthy,
   duplicate, and role-incompatible participants.

The existing aggregate engine path stays byte-identical because
`tasks[i].agent = NULL` (the new default) reproduces the current route. The
roundtable then builds on real multi-agent fan-out instead of an illusion of one.
(Note "byte-identical *engine* behavior," not "still works for users" — per §0.2
the aggregate command has no reachable caller today regardless of this fix.)

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

**The missing wiring (P0 must add this before its value lands).** Mirror the
existing typed `delegate.*` RPC family rather than overloading the bare
`delegate` method:

- **Thin client.**
  - Add `aggregate` (and later `roundtable`) to `delegate_arg_is_subcommand`
    (`cli_main.c:485`) so usage/help and subcommand detection work.
  - Add route entries in `cli_rpc_routes.inc` beside the other `delegate.*`
    rows (≈ `:235-243`):
    `{"delegate", "aggregate",  "delegate.aggregate",  marshal_delegate_aggregate,  NULL, <timeout>}`
    and `{"delegate", "roundtable", "delegate.roundtable", marshal_delegate_roundtable, NULL, <timeout>}`.
  - Add `marshal_delegate_aggregate` / `marshal_delegate_roundtable` that put
    **positional[0] → `prompt`** (not `role`, the bug above) and fold the flags
    (`--mode`, `--turns`, `--rounds`, `--apply`) into the request — modeled on
    `marshal_delegate` (`:1665`) minus the role-positional mapping.
- **Server.**
  - Register handlers in the dispatch table beside `{"delegate", handle_delegate}`
    (`src/server/server.c:1131`):
    `{"delegate.aggregate", handle_delegate_aggregate}` and
    `{"delegate.roundtable", handle_delegate_roundtable}`.
  - Implement those handlers **in a server-linked TU** (a new
    `src/server/delegate_ensemble_rpc.c` added to `SERVER_SRCS`, or inside
    `server_compute.c`). The body is the `cmd_agent_delegate.c:504-535` block
    rewritten as an RPC handler: read `prompt`/opts from the request, load
    config + `agent_config_t` server-side, gate on `ensemble_enabled`
    (and `roundtable_enabled`), call `delegate_ensemble_run` /
    `delegate_roundtable_run` (both already server-linked), and return the
    response plus `degraded` / `cost_capped` / `cost_usd` / `rounds_run` /
    `converged` as the RPC result.
  - Apply the same `persona`/prompt-length guards intentionally (the ensemble
    prompt is the *task*, so the `<20` guard is appropriate; just make the
    error name the ensemble, not a generic delegate).
- **MCP tool (parallel, optional in P0).** Expose `aggregate` / `roundtable`
  MCP tools in `server_mcp_delegate.c` mirroring `handle_mcp_delegate_call`, so
  IDE/agent callers can invoke the ensemble too.
- **`/v1` coverage.** The repo gates every RPC method on having a first-class
  `/v1` route or a documented exclusion (`v1-method-coverage-check`,
  `src/Makefile`). `delegate.aggregate` / `delegate.roundtable` are long-running
  and may stream, so either add `/v1` routes (`server_http_routes.inc`,
  `api/openapi-server-v1.yaml`, `cli_v1_routes_gen.inc`) or register a
  documented exclusion exactly as the foreground `delegate` method does.
- **Build + de-dup.** Register any new TU in `src/Makefile` (`SERVER_SRCS`) and,
  if applicable, the cmake profile; retire the lint-only `aggregate` block in
  `cmd_agent_delegate.c` (or keep it strictly as the platform CLI shim) so there
  is one live copy, not two divergent ones.

Until this wiring exists, the §0.1 routing fix repairs a path with no callers.
P0 therefore **wires the entry point and fixes the routing in the same phase**
(§7), with an end-to-end reachability test (CLI/RPC → engine → output) that
would have caught both this gap and the §0.1 bug.

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
      "stable_key": "short deterministic issue key",
      "location": "file:line or artifact section",
      "summary": "one-sentence issue"
    }
  ]
}
```

The engine treats `severity == "blocking"` plus `stable_key` as the deterministic
stop predicate. The aggregator may still semantically deduplicate and improve the
final prose, but the engine does not depend on semantic matching to decide
whether a new blocking issue appeared.

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
- **Draft mode — stability:** after the aggregator produces `Dₙ`, ask the
  `reason` role to score how much `Dₙ` differs from `Dₙ₋₁` on 0–100 (0 = no
  meaningful change). Stop when change `<` `roundtable_converge_threshold`
  (default 10). Cheap: one short judging call per round, same pattern as
  `agent_loop_parse_completion` (`agent_exec.h:144`).
- **Review mode — saturation:** stop when a round returns no feedback item with
  `severity == "blocking"` whose `stable_key` was not already raised in a prior
  round. Semantic dedup stays in the aggregator for final presentation, but the
  engine's stop predicate is deterministic and unit-testable.
- **Cost ceiling (shared, hard) — preflight, not only post-hoc.** The existing
  `ensemble_max_cost_usd` cap is a *running* total across all rounds. Today's
  single-shot path only checks cost *after* the fan-out has already spent it
  (`delegate_ensemble.c:169-173`), so the cap reports but never prevents. Over R
  rounds that compounds: post-hoc-only enforcement can overshoot by a full round ×
  N participants before the loop notices. This proposal therefore enforces **at
  the round boundary**: before dispatching round *n*, estimate that round's worst
  case from `(participants × effective_per_task_max_tokens) + aggregator + judge`
  at `ENSEMBLE_COST_PER_TOKEN`, and **do not start the round** if `accumulated +
  estimate` would cross the cap. `effective_per_task_max_tokens` is resolved from
  the task value when set, otherwise from the selected participant/provider
  default; a literal `0` cannot be treated as a zero-cost turn. The post-call
  accounting still runs to true up the real spend. Either way, cap handling
  returns the **best artifact so far** (§4, keep-best) with `cost_capped = 1`.
- **Size ceiling — bounded heap artifact, summarize-forward, no silent truncation.**
  Today the result is a fixed `char response[8192]` (`delegate_ensemble.h:12`) and
  synthesis a `char synthesis_buf[16384]` (`:215`), both silently
  `snprintf`-truncated (`:181/200/233`) — the exact failure mode a growing
  multi-round draft hits. The roundtable does not inherit this: `roundtable_result_t`
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
   int converge_threshold;  /* 0-100 draft-delta stop, default 10 */
   int apply_review;        /* mode B: final draft turn applies the review */
} roundtable_opts_t;

typedef struct {
   char *artifact;          /* heap, grown to fit; caller frees. best round (§3) */
   int rounds_run;
   int converged;           /* 1 = stopped on convergence, 0 = hit cap/rounds */
   int degraded;            /* a round fell back to best-candidate, or summarized */
   int truncated;           /* a peer note was summarized forward to fit budget */
   int cost_capped;         /* running cost cap hit (preflight or post-hoc) */
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
the draft *worse*, and the convergence judge only detects *low change*, not
regression. So each round's aggregated artifact is scored (the same `reason`-role
delta judge, scored against the task rather than the prior draft), the engine
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
golden parity tests prove the same response text, `success`, `degraded`,
`cost_capped`, response-size limit, aggregation max tokens, and `cost_usd`
semantics for the existing one-shot cases. Once that is proven, `aggregate` can
be documented as the one-round roundtable special case.

`build_synthesis_prompt` may be refactored into a parameterized prompt builder so
the "you are a synthesis aggregator" framing (draft merge) and a new "you are
consolidating peer reviews" framing (review dedup) share one code path. The
refactor must preserve the current aggregate prompt exactly until parity tests
permit changing the implementation.

## §5 CLI

A new subcommand built on the §0.2 wiring (thin-client route +
`marshal_delegate_roundtable` + server `handle_delegate_roundtable`), using the
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
  `converged`, marshalled from the RPC result.
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
| `roundtable.converge_threshold` | 10 | draft-delta stop (0–100) |
| `roundtable.turns` | `parallel` | default turn discipline |

Participants (`ensemble.reference_models`), aggregator
(`ensemble.aggregator`), `min_successful`, and `max_cost_usd` are reused as the
configuration source, with the participant-routing fix from §0.1. The new
roundtable defaults are serialized as a **top-level** `roundtable` object:

```yaml
roundtable:
  max_rounds: 3
  converge_threshold: 10
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

- `config.h` — three new fields on `config_t` (`roundtable_max_rounds`,
  `roundtable_converge_threshold`, `roundtable_turns[16]`).
- `config.c` — defaults (3, 10, `"parallel"`), beside the ensemble defaults at
  `config.c:507-509`.
- `config_sections.c` — add a sibling inline parser for the top-level
  `roundtable` object, near the existing ensemble parser.
- `config_save.c` — extend the inverse saver for round-trip of the top-level
  `roundtable` object.
- `config_fields.c` — register **only** the three flat scalars so `aimee config
  get/set roundtable.max_rounds` works. (The array-valued `ensemble.*` keys stay
  inline-only, as today.)

This is a deliberate, stated decision rather than an inherited convention: scalars
are CLI-settable; the array participant list is not.

## §7 Phasing

1. **P0 — wire the entry point (§0.2) + fix the routing bug (§0.1) + the `srand`
   reseed.** These ship together because the routing fix is invisible without a
   reachable caller.
   - *Wiring (§0.2):* add the `delegate.aggregate` (and `delegate.roundtable`
     placeholder) thin-client route + marshal (`prompt` as positional, not
     `role`), the server dispatch-table entries, and a server-linked
     `handle_delegate_aggregate` that calls `delegate_ensemble_run`; add the
     `/v1` route or documented exclusion; retire the lint-only `aggregate` block.
   - *Routing (§0.1):* thread an optional `agent` selector through
     `agent_task_t` and a shared named-agent execution helper, resolving named
     agents the way `aux_router.c:51-58` already does; point each ensemble
     fan-out task at `ensemble_reference_models[i]`. Move `srand` out of the
     per-call path.
   - *Tests:* an **end-to-end reachability test** (CLI/RPC → engine → output)
     that fails on today's tree — this is the test whose absence let both the
     unreachability and the §0.1 bug ship. Plus **un-stubbed routing tests**:
     three configured references → three distinct `agent_name` results,
     missing/disabled/unhealthy participants, duplicate names, role
     incompatibility, and proof that parallel named-agent calls do not race on
     shared `agent_t` mutation. The existing stubbed ensemble tests also re-run
     green.

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
3. **P2 — convergence + sequential turns.** Add the `reason`-role delta judge
   (dedicated judge prompt emitting the `{"completion":N}` shape that
   `agent_loop_parse_completion` already parses, `agent_exec.h:144` — the stock
   `reason` template alone does not emit that shape) and `--turns sequential`
   (per-round shuffle via the fixed RNG). Tests for early-stop on a stable draft,
   keep-best on a regressing draft, and order-shuffling.
4. **P3 — review mode.** `--mode review`, structured-feedback JSON prompt,
   `stable_key`-based blocking-issue saturation stop, aggregator dedup, optional
   `--apply` final draft turn. Malformed reviewer JSON counts as a failed
   participant after one optional repair prompt; the engine computes a normalized
   fallback key from `severity/category/location/summary` when `stable_key` is
   missing, but it never lets malformed JSON silently satisfy convergence. Tests
   for saturation stop, dedup, malformed JSON, and normalized fallback keys.
5. **P4 — CLI + config + docs.** `aimee delegate roundtable`, the `roundtable.*`
   config keys (inline parse/save + scalar registration in `config_fields.c`, §6),
   `MANUAL.md` section. Live-validate against the configured delegates (minimax /
   mimo-2.5 / mistral) — now genuinely distinct after P0 — drafting and then
   reviewing a small real proposal end-to-end.

## §8 Risks / non-goals

- **Cost.** R rounds × N participants × turn cost is roughly R× a single MoA
  call. Bounded by the **preflight** running cost cap (§3, refuses to start a round
  that would cross the cap — not just post-hoc reporting), a low default
  `max_rounds = 3`, and early convergence stops. The CLI prints accumulated
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
- **New typed RPC methods, but not a new transport or service.** §0.2 adds two
  typed methods (`delegate.aggregate`, `delegate.roundtable`) to the existing
  RPC dispatch and `/v1` surface — the same kind of seam the rest of the
  `delegate.*` family already uses. It adds no new transport/protocol, no
  long-lived service, no persistence, and no new background job type. A
  roundtable is a single foreground `delegate`-family invocation. (The earlier
  claim that this "adds no RPC" was wrong: the feature has *no* reachable RPC
  today, which is exactly the gap §0.2 closes.)
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
generalizing here forces the fix of three latent defects the shipped engine has
been quietly carrying — its complete unreachability (§0.2), the unrouted
references (§0.1), and the per-call `srand` reseed (§4).
