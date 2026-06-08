# Proposal: Agent roundtable — round-robin collaborative drafting and review

- **State:** draft — pending review
- **Author:** JBailes
- **Date:** 2026-06-08
- **Charter roles:** Draft (initial + revision turns), Review (per-round
  critique + final synthesis/aggregation), Reason (convergence judging).
- **Scope:** `src/server/delegate_ensemble.c` (the engine being generalized — add
  a multi-round loop alongside the existing single-shot path),
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
fan-out / cost-cap / degrade-to-best machinery, wrapped in a bounded loop where
each round's shared artifact feeds the next round's prompts. It reuses
everything that already works and adds only the loop, the turn discipline, and
the convergence check.

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
  reuses the participant list and aggregator verbatim; it only **adds** round
  count and a convergence threshold.
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

So the net new code is: **a bounded loop around the existing fan-out, a
shared-artifact prompt builder, and a convergence check.** Nothing else.

## §1 What a roundtable is

A roundtable runs up to `R` rounds over a shared artifact `D` (the draft).
Participants are the configured reference models. Two collaboration *modes*,
selected per invocation:

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
- **Review mode — saturation:** stop when a round returns no feedback item tagged
  `blocking` that wasn't already raised in a prior round (string/semantic dedup
  in the aggregator).
- **Cost ceiling (shared, hard):** the existing `ensemble_max_cost_usd` cap is
  now a *running* total across all rounds. The moment the accumulated estimate
  (`estimate_cost` summed per round + aggregator turns) would exceed the cap, the
  loop stops and returns the **best artifact so far** with `cost_capped = 1` —
  the exact degrade path `delegate_ensemble.c:173-187` already implements, lifted
  to the loop level.

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
build per-participant tasks (now each task's `user_prompt` is composed of the
task + current artifact + peer notes, via a new `build_round_prompt` that
generalizes `build_synthesis_prompt`), fan out (parallel or sequential per §2),
apply cost/min-success/degrade exactly as today, then call `run_aggregator` to
fold the round into the next artifact. After each round, run the convergence
check (§3) and break early. **The current `delegate_ensemble_run` becomes the
`max_rounds == 1, mode == DRAFT` special case** — we can even reimplement it as a
one-line wrapper to prove the generalization is faithful, keeping
`aimee delegate aggregate` behavior byte-identical.

`build_synthesis_prompt` is refactored into a parameterized prompt builder so the
"you are a synthesis aggregator" framing (draft merge) and a new "you are
consolidating peer reviews" framing (review dedup) share one code path.

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
(`ensemble.aggregator`), `min_successful`, and `max_cost_usd` are **reused
unchanged**. New fields land in `config.h`, `config.c` (defaults),
`config_fields.c` (registration), `config_sections.c` (parse), and
`config_save.c` (round-trip) — the same five-file pattern every config field in
this tree follows.

## §7 Phasing

1. **P1 — engine generalization (`mode draft`, `turns parallel`).** Add
   `delegate_roundtable_run` looping the existing round; refactor
   `build_synthesis_prompt` → parameterized `build_round_prompt`; reimplement
   `delegate_ensemble_run` as the `rounds==1` wrapper to prove parity. Running
   cost cap + degrade-to-best at loop level. Unit tests sibling to
   `test_delegate_ensemble.c` (stubbed `agent_run_parallel`): convergence stop,
   cap stop, degrade path, parity with old `aggregate` output.
2. **P2 — convergence + sequential turns.** Add the `reason`-role draft-delta
   judge and `--turns sequential` (per-round shuffle). Tests for early-stop on a
   stable draft and order-shuffling.
3. **P3 — review mode.** `--mode review`, structured-feedback prompt, blocking-
   issue saturation stop, aggregator dedup, optional `--apply` final draft turn.
4. **P4 — CLI + config + docs.** `aimee delegate roundtable`, the `roundtable.*`
   config keys, `MANUAL.md` section. Live-validate against the configured
   delegates (minimax / mimo-2.5 / mistral) drafting and then reviewing a small
   real proposal end-to-end.

## §8 Risks / non-goals

- **Cost.** R rounds × N participants × turn cost is roughly R× a single MoA
  call. Mitigated by the running cost cap (§3, hard) and a low default
  `max_rounds = 3`. The CLI prints accumulated `cost_usd` like `aggregate` does.
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
family, makes the existing `aggregate` a provable special case, and confines the
new surface to "a loop + a prompt builder + a convergence check."
