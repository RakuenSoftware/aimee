# Proposal: Agent roundtable — round-robin collaborative drafting and review

- **State:** draft — pending review
- **Author:** JBailes
- **Date:** 2026-06-08
- **Charter roles:** Draft (initial + revision turns), Review (per-round
  critique + final synthesis/aggregation), Reason (convergence judging).
- **Scope:** `src/server/delegate_ensemble.c` (the engine being generalized — add
  explicit participant routing plus a multi-round loop alongside the existing
  single-shot path),
  `src/headers/delegate_ensemble.h` (new `delegate_roundtable_*` surface),
  `src/cmd_agent_delegate.c` (new `aimee delegate roundtable` subcommand beside
  the existing `aggregate`), config plumbing
  (`src/headers/config.h`, `src/config.c`, `src/config_fields.c`,
  `src/config_sections.c` — a `roundtable.*` section reusing the `ensemble.*`
  knobs), `src/config_save.c`, the existing parallel/sequential agent primitives
  in `src/headers/agent_exec.h` (`agent_run_parallel`,
  `agent_run_with_tools_write_enforce`), unit tests
  (`src/tests/test_delegate_ensemble.c` sibling), docs (`MANUAL.md`). No new
  long-lived service, no new RPC transport, no new provider integration.

## Goal

Let **several agents collaborate on one artifact over multiple turns** — taking
turns to draft, revise, and critique a proposal (or any document) while each one
can see what the others produced in the previous round. The artifact converges
through iteration instead of being assembled in a single shot.

Aimee already has the *single-shot* version of this: **Mixture-of-Agents**
(`aimee delegate aggregate "<prompt>"` → `delegate_ensemble_run`,
`src/server/delegate_ensemble.c`). MoA fans the same prompt to N diverse
reference models **in parallel**, then runs **one** synthesis pass that
reconciles their answers into a final response. It is the right primitive, but it
is intentionally flat:

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
existing orchestration shape, but it also closes one current implementation gap:
configured `ensemble.reference_models` must become real routed participants, not
only labels in the synthesis prompt.

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
  reuses the configured participant list and aggregator, but P0/P1 must make the
  reference list executable: today `delegate_ensemble_run` sets each fan-out task's
  `role = NULL` and only uses `ensemble_reference_models` as display names in the
  synthesis prompt. Roundtable implementation must route each configured
  participant by explicit agent/model name or by a new `agent_task_t` participant
  field before claiming true multi-agent collaboration.
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

So the net new code is: **participant routing, a bounded loop around the existing
fan-out, bounded prompt/artifact handling, a shared-artifact prompt builder, and
a convergence check.**

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
- **Cost ceiling (shared, hard):** the existing `ensemble_max_cost_usd` cap is
  now a *running* total across all rounds. The first implementation can enforce
  this post-call, matching today's `delegate_ensemble_run`: estimate fan-out cost
  after a round returns, add aggregator/judge costs after those calls return, and
  stop before starting another round once the accumulated estimate has crossed the
  cap. If a later phase wants true preflight enforcement, it must reserve budget
  from prompt size and max-token limits before dispatch. In either case, cap
  handling returns the **best artifact so far** with `cost_capped = 1`.
- **Size ceiling:** prompts and artifacts are bounded. Current synthesis uses a
  16KB prompt buffer and `delegate_ensemble_result_t.response` is 8KB; roundtable
  prompts can grow by `task + artifact + peer notes + revisions`. P1 must define a
  truncation or summary policy before dispatch, preferably reusing existing
  delegate token-budget/context-shedding helpers, and must return a controlled
  degrade result rather than failing on ordinary long peer outputs.

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
   char artifact[16384];    /* final draft (A) or consolidated review (B) */
   int rounds_run;
   int converged;           /* 1 = stopped on convergence, 0 = hit cap */
   int degraded;            /* a round fell back to best-candidate */
   int cost_capped;         /* running cost cap hit */
   double cost_usd;         /* accumulated across all rounds + aggregators */
} roundtable_result_t;

int delegate_roundtable_run(agent_config_t *acfg, const config_t *cfg,
                            const char *task, const roundtable_opts_t *opts,
                            roundtable_result_t *out);
```

`delegate_roundtable_run` is a loop whose body is *the existing ensemble round*:
build per-participant tasks (each task is routed to a configured participant, and
each task's `user_prompt` is composed of the task + current artifact + peer notes
via a bounded `build_round_prompt`), fan out (parallel or sequential per §2),
apply cost/min-success/degrade with the same semantics as today, then call
`run_aggregator` to fold the round into the next artifact. After each round, run
the convergence check (§3) and break early.

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
configuration source, with the participant-routing fix from §0. New fields land
in `config.h`, `config.c` (defaults),
`config_fields.c` (registration), `config_sections.c` (parse), and
`config_save.c` (round-trip) — the same five-file pattern every config field in
this tree follows.

## §7 Phasing

1. **P0 — executable participant routing.** Make `ensemble.reference_models`
   select the actual participant for each fan-out task instead of acting only as
   candidate labels. This can be a per-task agent/model field, an explicit
   temporary `default_agent` override per task, or another narrow routing helper.
   Tests must prove three configured references invoke three distinct configured
   participants when available.
2. **P1 — engine generalization (`mode draft`, `turns parallel`).** Add
   `delegate_roundtable_run` looping the existing round; add bounded
   `build_round_prompt`; keep `delegate_ensemble_run` frozen while extracting
   shared helpers. Running cost cap + degrade-to-best at loop level. Unit tests
   sibling to `test_delegate_ensemble.c` (stubbed `agent_run_parallel`):
   convergence stop, post-round cap stop, degrade path, prompt/artifact
   truncation, and aggregate golden parity for the existing public path.
3. **P2 — convergence + sequential turns.** Add the `reason`-role draft-delta
   judge and `--turns sequential` (per-round shuffle). Tests for early-stop on a
   stable draft and order-shuffling.
4. **P3 — review mode.** `--mode review`, structured-feedback JSON prompt,
   `stable_key`-based blocking-issue saturation stop, aggregator dedup, optional
   `--apply` final draft turn.
5. **P4 — CLI + config + docs.** `aimee delegate roundtable`, the `roundtable.*`
   config keys, `MANUAL.md` section. Live-validate against the configured
   delegates (minimax / mimo-2.5 / mistral) drafting and then reviewing a small
   real proposal end-to-end.

## §8 Risks / non-goals

- **Cost.** R rounds × N participants × turn cost is roughly R× a single MoA
  call. Mitigated by the running cost cap (§3), a low default `max_rounds = 3`,
  and explicit post-round stop semantics. The CLI prints accumulated `cost_usd`
  like `aggregate` does.
- **Routing ambiguity.** The current ensemble implementation labels candidates
  with `ensemble.reference_models` but does not route fan-out tasks to those
  names. P0 exists so the roundtable cannot accidentally run repeated calls to
  the default routed agent while presenting them as distinct peers.
- **Prompt growth.** Multi-round prompts can exceed the current fixed buffers
  quickly. P1 must bound prompt and artifact sizes before dispatch and test the
  truncation/degrade behavior.
- **Non-convergence / drift.** A pathological round could make the draft worse;
  the convergence judge stops on *low* change but cannot detect *regression*. P2
  can optionally keep the highest-scored intermediate artifact (the same
  `best_candidate` idea, scored by the aggregator) rather than the last one.
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
tests, and confines the new surface to participant routing, a loop, bounded prompt
assembly, and a convergence check.
