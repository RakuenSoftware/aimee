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
  per-task participant selector through `agent_task_t` → `parallel_worker` →
  `agent_run` so fan-out tasks route to distinct configured agents — see §0.1),
  `src/cmd_agent_delegate.c` (new `aimee delegate roundtable` subcommand beside
  the existing `aggregate`), config plumbing
  (`src/headers/config.h`, `src/config.c`, `src/config_sections.c` for the inline
  `ensemble.*`/`roundtable.*` parse, `src/config_save.c` for round-trip, and
  `src/config_fields.c` only for the new flat scalar `roundtable.*` keys so they
  are `aimee config get/set`-able — see §6), the existing parallel/sequential
  agent primitives in `src/headers/agent_exec.h` (`agent_run_parallel`,
  `agent_run_with_tools_write_enforce`), the per-task provider/model resolution
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
existing orchestration shape, and as step one it **fixes a shipped bug** (§0.1):
configured `ensemble.reference_models` are today only labels in the synthesis
prompt — every "participant" is the same default agent — and must become real
routed participants before any of this is meaningful.

## §0 What already exists (so we don't rebuild it)

The hard parts — parallel execution, cost accounting, graceful degradation,
config plumbing, a CLI entry point — are done. Confirmed in the tree:

- **The MoA engine.** `delegate_ensemble_run`
  (`src/server/delegate_ensemble.c:138`) already does: build N tasks → run them
  with `agent_run_parallel` (`agent_exec.h:41`) → estimate cost
  (`estimate_cost`, `ENSEMBLE_COST_PER_TOKEN`) → enforce a hard cost cap
  (`cfg->ensemble_max_cost_usd`) → require a minimum number of successes
  (`ensemble_min_successful`, default 2) or **degrade to the single best
  candidate** (`best_candidate`, by length) → shuffle candidate order to kill
  position bias (`shuffle_indices`, `delegate_ensemble.c:21`) → run a synthesis
  pass (`run_aggregator`, configurable `role@`-prefixed aggregator). **Every one
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
- **The CLI seam exists.** `aimee delegate aggregate "<prompt>"`
  (`src/cmd_agent_delegate.c:504-535`) already loads config, checks
  `ensemble_enabled`, loads `agent_config_t`, calls the engine, and prints the
  result with degrade/cost warnings. A `roundtable` subcommand is a near-clone of
  this block pointing at the new entry point.
- **Both execution shapes are available.** `agent_run_parallel`
  (`agent_exec.h:41`) for "all participants act this round simultaneously," and
  `agent_run_with_tools_write_enforce` (`agent_exec.h:37`, already used by
  `run_aggregator`) for a single sequential turn. A *true* round-robin
  (A → B → C, each seeing the prior) is just sequential `agent_run_*` calls in a
  loop; a *parallel-per-round* roundtable is `agent_run_parallel` per round. We
  support both (see §2).
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

So the net new code is: **the routing fix (§0.1), a bounded loop around the
existing fan-out, a bounded heap artifact, a shared-artifact prompt builder, and
a convergence check.**

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
2. Honor it in `parallel_worker` (`agent_runtime.c:386-391`) and in `agent_run`:
   when `task->agent` is set, resolve it the way `aux_router.c:51-58` already
   resolves a per-task `provider`/`model` (that file is the existing precedent for
   "this unit of work runs on a specific named provider/model"), bypassing the
   role fallback chain and routing directly to the named agent.
3. `delegate_ensemble_run` sets `tasks[i].agent = cfg->ensemble_reference_models[i]`
   so the N references become N real participants.
4. **Un-stubbed test.** `test_delegate_ensemble.c` currently stubs
   `agent_run_parallel` wholesale, which is exactly why this bug shipped unseen.
   P0 adds a routing test that exercises the real selector resolution (three
   configured references → three distinct `agent_name` results) instead of stubbing
   it away.

The existing aggregate path keeps working because `tasks[i].agent = NULL` (the
new default) reproduces the current route. The roundtable then builds on real
multi-agent fan-out instead of an illusion of one.

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
  of `agent_run_with_tools_write_enforce` calls. Maximum cross-pollination
  (later speakers react to earlier ones immediately) at the cost of serial
  latency. To avoid a fixed speaking order biasing the result, the participant
  order is **shuffled each round** with the existing `shuffle_indices`
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
  case from `(participants × per-task max_tokens) + aggregator + judge` at
  `ENSEMBLE_COST_PER_TOKEN`, and **do not start the round** if `accumulated +
  estimate` would cross the cap. The post-call accounting still runs to true up the
  real spend. Either way, cap handling returns the **best artifact so far** (§4,
  keep-best) with `cost_capped = 1`.
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
   int converged;           /* 1 = stopped on convergence, 0 = hit cap */
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

A new subcommand mirroring the `aggregate` block (`cmd_agent_delegate.c:504`):

```
aimee delegate roundtable "<task or path to artifact>" \
    [--mode draft|review] [--turns parallel|sequential] \
    [--rounds N] [--apply]
```

- Reuses the same guard (`ensemble_enabled` → reuse, or a sibling
  `roundtable_enabled`; recommend gating on the existing `ensemble_enabled` so
  there is one "multi-agent is on" switch).
- Prints the final artifact; on `--mode review` prints the consolidated review;
  emits the same degrade/cost warnings the `aggregate` path already prints
  (`cmd_agent_delegate.c:530-532`), plus `rounds_run` / `converged`.
- `aimee delegate aggregate` stays as the documented one-shot alias.

## §6 Config (reuse the ensemble section)

Extend the parsed `ensemble.*` / new `roundtable.*` section
(`config_parse_ensemble_section`, `src/config_sections.c:1068`) with:

| key | default | meaning |
|-----|---------|---------|
| `roundtable.max_rounds` | 3 | hard round cap |
| `roundtable.converge_threshold` | 10 | draft-delta stop (0–100) |
| `roundtable.turns` | `parallel` | default turn discipline |

Participants (`ensemble.reference_models`), aggregator
(`ensemble.aggregator`), `min_successful`, and `max_cost_usd` are reused as the
configuration source, with the participant-routing fix from §0.1.

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
- `config_sections.c` — extend the existing inline ensemble section parser to read
  the `roundtable.*` keys (one parser, one section).
- `config_save.c` — extend the inverse saver for round-trip.
- `config_fields.c` — register **only** the three flat scalars so `aimee config
  get/set roundtable.max_rounds` works. (The array-valued `ensemble.*` keys stay
  inline-only, as today.)

This is a deliberate, stated decision rather than an inherited convention: scalars
are CLI-settable; the array participant list is not.

## §7 Phasing

1. **P0 — fix the routing bug (§0.1) + the `srand` reseed.** Thread an optional
   `agent` selector through `agent_task_t` → `parallel_worker` → `agent_run`,
   resolving named agents the way `aux_router.c:51-58` already does; point each
   ensemble fan-out task at `ensemble_reference_models[i]`. Move `srand` out of
   the per-call path. **Un-stubbed routing test** (three configured references →
   three distinct `agent_name` results), plus the existing stubbed ensemble tests
   re-run green. This phase ships independently and repairs the marked-Done MoA
   feature on its own; everything below builds on real fan-out.
2. **P1 — engine generalization (`mode draft`, `turns parallel`).** Add
   `delegate_roundtable_run` looping the existing round; add budget-bounded
   `build_round_prompt` (summarize-forward, no silent truncation, §3); heap
   `char *artifact` with keep-best-so-far (§4); **preflight + post-hoc** running
   cost cap at loop level (§3). Keep `delegate_ensemble_run` frozen while
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
   `--apply` final draft turn. Tests for saturation stop and dedup.
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
  P0 fixes this with a per-task `agent` selector and proves it with an un-stubbed
  test, so the roundtable runs genuinely distinct peers rather than repeated calls
  to one default agent presented as a panel.
- **Prompt growth — handled by design.** Per-round prompts are assembled under an
  explicit byte budget; over-budget peer notes are summarized forward (one
  aggregator pass) and flagged `truncated`, never silently `snprintf`-cut. The
  artifact is heap-grown rather than a fixed buffer (§3, §4).
- **Non-convergence / drift — kept-best by default.** A pathological round can make
  the draft worse, and the convergence judge detects only *low change*, not
  *regression*. The engine therefore scores each round and returns the
  **highest-scored** artifact (`best_round`), not the last (§4). This is default
  behavior, not an opt-in.
- **Not a new transport or service.** This is strictly an orchestration loop over
  existing agent execution; it adds no RPC, no persistence, no background job
  type. A roundtable is a single foreground `delegate` invocation, like
  `aggregate` today.
- **Not the self-correcting `agent_loop`.** That loop is *one* agent improving
  its own output; the roundtable is *many* agents collaborating. They share stop-
  condition machinery but are deliberately separate engines.

## §9 Why adapt the ensemble rather than build new

The ensemble already encodes the three things that are easy to get wrong in
multi-agent orchestration — **bounded cost, graceful degradation when providers
fail, and position-bias control** — and it is already CLI- and config-wired. A
greenfield "debate" engine would re-derive all of that. Generalizing
`delegate_ensemble.c` into a loop keeps one engine, one config section, one CLI
family, makes the existing `aggregate` a provable special case after parity
tests, and confines the new surface to the §0.1 routing fix, a loop, budget-bounded
prompt assembly, and a convergence check. As a bonus, generalizing here forces the
fix of two latent defects in the shipped engine — the unrouted references (§0.1)
and the per-call `srand` reseed (§4) — that the single-shot path has been quietly
carrying.
