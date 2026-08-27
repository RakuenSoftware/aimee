# Proposal: Governed recall: a decision record for every candidate, and adversarial measurement of the gates that produce it

- **State:** PENDING. Concepts and design only; no code in this PR. Eight
  transferable concepts, each scoped as **reuse/extend what exists** and each
  anchored to a verified surface in the current tree.
- **Author:** JBailes
- **Date:** 2026-08-27
- **Charter roles:** Observe-Record (recall decision record), Enforce (release
  gate, re-authorization, erasure state), Constrain-Verify (invariants, semantic
  fuzz), Measure (trajectory-level adversarial benchmark, claim register)
- **Related pending work:**
  [`proposal-evidence-provenance-tiers.md`](proposal-evidence-provenance-tiers.md)
  (classifies and gates untrusted memory; §2, §3 and §7 below amend its stated
  scope), [`local-first-memory-and-trust-patterns.md`](local-first-memory-and-trust-patterns.md)
  (IPI/drift threat model).

---

## Thesis

Aimee's memory path already **decides** a great deal on the way to a prompt: it
scores, orders, drops low-confidence candidates, withholds sensitive facts,
abstains under threshold, and suppresses outcome-demoted rows. What it does not
do is **record those decisions**, and what the benchmarks do not do is measure
them **over a trajectory** rather than a single turn.

Two consequences follow, and they compound:

1. **"Why did the agent say that?" has no answer, and neither does "why did it
   not?"** A candidate that was dropped leaves no trace. Every gate on the recall
   path is therefore individually untestable in production and only weakly
   testable in a benchmark, because the only observable is the final answer.
2. **The gates are measured single-shot, so their real cost is invisible.** The
   damage a bad memory does is not that it loses one question; it is that it
   re-fires in every later step of the same task. A per-question accuracy delta
   cannot see that, and cannot see which gate paid for the recovery.

The proposal is a single foundation plus seven things it unlocks:

> **Every candidate that reaches the recall path leaves a decision record: a
> verdict, a reason, and its provenance. Serving is recorded, not only writing.**

Everything else here (the release gate, evidence re-authorization, erasure
tombstones, the channel-sensitivity axis, the trajectory benchmark, the
invariants file, the claim register) either produces a reason code or consumes
one. §1 is the prerequisite; §2–§9 are separately acceptable once it exists.

---

## §0 What already exists (DRY map)

Verified against `a397223849`. Nothing below is inferred from a file name.

| Piece | Existing surface | State |
| --- | --- | --- |
| Assembly chokepoint | `emit_candidate` (`memory_assemble.c:387`), called once from the section loop (`:1283`) | **exists**: the single point every served candidate passes |
| Abstention | `answerability_withheld` / `config_memory_abstain_enabled` (`memory_assemble.c:999`, `:1082`, `:1106`, `:1354`), with `memory_directive_record_retrieval_failure` on threshold crossings | **exists**: recall-side abstain is already first-class |
| Per-attribute sensitivity gate | `memory_pii_should_inject` / `memory_pii_rel_sensitivity` (`src/modules/memory/memory_pii_gate.c`), wired at `src/modules/db2/c/fact_recall.c:102` and `src/kb/db2_adapters/kb_service_backend_memory.c:1430` | **exists**, fails closed on unknown `rel_type` (`rel_types.sensitivity NOT NULL DEFAULT 'pii'`, `schema.sql:2052`) |
| Typed-triple write gate | `memory_fact_gate_check` + `FACT_GATE_REJECT_SENSITIVE` (`src/modules/memory/memory_fact_gate.h`) | **exists**: single commit point for semantic edges |
| Supersession + conflict | `memory_supersede` (`memory_advanced.c:55`, `:286`), `memory_conflict.c`, `db2_memory_supersede_lookup` (`memory_assemble.c:433`) | **exists** |
| **Destructive-edit authority** | `memory_authority_t` (`src/headers/memory_authority.h`); `memory_delete_as` / `memory_update_content_as` (`headers/memory.h:412`, `:425`) route MODEL authority to `memory_retire` / `memory_supersede` and only USER authority destroys, behind `CAP_MEMORY_ADMIN` (`headers/server.h:156`) | **exists**: the tree already refuses to let a model destroy what a user stated |
| **Persisted write provenance** | `memory_insert_ex` stores the write's authority as `memories.provenance_category` via `MEMORY_PROVENANCE_FOR` (`memory_core_crud.c:493`), fail-closed to `agent_message` (`schema.sql:605`), derived from the calling surface and never from a request field | **exists and is now populated** |
| Lineage records | `memory_lineage_insert(object_type, object_id, source_kind, source_ref, confidence)` (`headers/memory.h:497`), `memory_provenance` table | **exists**: per-object origin, one row per assertion |
| Tamper-evident ledger | `audit_worm_append` (`src/modules/audit/audit_worm.c:135`) plus `_verify` / `_checkpoint` / `_seal` / `_read_page` (`docs/modules/audit.md`) | **exists**, and is stronger than what §1 needs |
| Write-side audit | `mem_audit("memory.insert" \| ".merge" \| ".update" \| ".reject" \| ".delete", …)` (`memory_core_crud.c:409`-`:652`), documented as **non-content fields only** (`headers/memory.h:428`) | **exists**: mutations only |
| Action authorization boundary | `policy_check_tool` (`src/server/execution_policy_bus.c:26`, declared `headers/agent_exec.h:236`), consumed by `pre_tool_check` | **exists**: authorizes the *action* |
| Poison benchmark | `benchmarks/memory/poison_gate.py` + `poison_fixtures.json` | **exists**: two scenarios, single-turn |
| Recall benchmarks | `benchmarks/locomo/`, `benchmarks/longmemeval/` (direct + LLM tracks, external anchors documented) | **exists**, with anchor discipline |

Two of these landed recently and change what this proposal needs to argue. The
authority split already establishes that **who is asking** governs whether an
edit destroys, and `provenance_category` already records **what kind of caller
wrote a row**, fail-closed. Both are write-side. Neither is readable as a reason
on the recall path, which is the gap below.

### The verified gaps

- **No recall decision record anywhere.** `memory_assemble.c` contains **32
  `continue;` statements**, none of which records why the candidate went away;
  `src/modules/db2/c/fact_recall.c:102` drops a PII-gated fact the same way; and
  `answerability_withheld` zeroes `cand_count` wholesale. Grepping
  `src/modules/memory/` for `exclusion`, `drop_reason`, `why_not`,
  `recall_trace`, or `memory_trace` returns nothing but the `answerability_*`
  locals and gate comments. Every gate is silent by construction.
- **Audit records writes, not serves.** `mem_audit` fires on insert, merge,
  update, reject and delete, and its contract is explicitly non-content.
  Nothing records that a memory was *injected into a prompt*, nor that one was
  *withheld*. The WORM ledger is ready to carry it.
- **Erasure is still an event, not a state, on the path that erases.**
  `memory_delete_as` (`memory_core_crud.c:621`) is correctly split: MODEL
  authority retires (the row survives under `key#vN` with `valid_until`
  stamped, readable through `memory_fact_history`), USER authority hard-deletes.
  Retirement is versioning, and it deliberately **keeps** the content. The
  destructive path (`memory_delete`, `:632`, reached from `memory op forget` via
  `mem_delete` and `kb_client_memory_delete`) wipes provenance, drops the
  record's own and every unit-scoped pgvector point, and deletes the row,
  leaving **nothing behind**. So a restore resurrects the content and a
  re-ingest of the same text is stored as a fresh, clean, high-confidence
  memory. Neither branch gives recall a reason to state.
- **Corroboration is intra-memory, not cross-source.** The "≥2 corroborating
  units per memory" filter in `memory_collect_unit_matches_via_vector`
  (`memory_core_search_b.c:1251`) groups vector hits by *parent memory_id*. It
  says "this memory matched in two places", never "two independent sources
  asserted this value". `memory_lineage` holds the data for the second question;
  nothing asks it.
- **Poisoning is measured single-shot.** `poison_gate.py` runs a fixed
  `clean`/`poison` pair, scores independent questions by token overlap, and
  gates on `clean_accuracy - poison_accuracy <= delta_max`. There is no
  multi-step trajectory, no per-trajectory pairing, and no attack taxonomy, so
  the re-firing cost of a surviving poison is not measured at all.
- **The action boundary re-checks the action, not the evidence.**
  `policy_check_tool` authorizes a tool call. Nothing asks whether the memory
  the plan rested on is *still* servable at the moment the side effect fires.

---

## Part II: Implementation plan

### §1 The recall decision record (Observe-Record): the foundation

Every candidate entering the recall path exits with exactly one of:

- `served`: plus its provenance (source_kind/source_ref, tier, confidence)
- `withheld`: plus a **reason code** from a closed vocabulary
- `abstained`: the whole-turn case, with the threshold and candidate count

Reason codes are a small fixed enum, not free text, because they are assertion
targets in tests and grouping keys in the benchmark. The initial vocabulary is
exactly what the code already implements silently: `sensitivity_withheld`,
`below_confidence_floor`, `superseded`, `outcome_demoted`, `wrong_scope`,
`budget_exhausted`, `truncated`, `duplicate`, `retention_expired` (new, §8),
`erased` (new, §7), `tier_not_main` (from the tier proposal), plus
`answerability_withheld` for the turn-level case.

Two invariants, and they are the point of the section:

> **Every served candidate carries provenance. Every withheld candidate carries
> a reason. A drop site with no reason code is a bug, and CI can see it.**

Placement follows the existing chokepoint: `emit_candidate` is already the sole
serialization point, so the record is emitted alongside it and the bare
`continue;` sites on the candidate path become reason-carrying rejects. The record has two sinks with
different lifetimes:

- **Turn-local trace**, cheap and always on, readable by `aimee memory` /
  trace surfaces. This is what makes "why did it say that?" answerable.
- **WORM ledger**, gated (`audit_recall_enabled`, default off), for
  deployments that need serve-side evidence. `audit_worm_append` already
  exists and the ledger already carries governed-action evidence; this adds
  producers, not a store.

Cost control matters: the trace is bounded per turn (a fixed-size ring of
records, oldest evicted with a counter), and the WORM sink records *decisions*,
not content.

### §2 Trajectory-level adversarial measurement (Measure)

Extend `benchmarks/memory/` from a two-scenario retrieval gate to a
**trajectory benchmark**, keeping the existing fixture format as one family.

Three changes, in order of value:

1. **A scenario is a sequence, not a question set.** Ingest turns, then *n*
   query steps against the same subject. This is what exposes the metric the
   current gate cannot see:

   > **Compounding-error rate**: given one bad memory that survives the gates,
   > in how many *later* steps of the same trajectory does it re-fire?

   A single-turn benchmark scores that as one loss. It is not one loss; it is
   the loss multiplied by the remaining step count, which is precisely why a
   memory-layer defect is worse than a retrieval miss.

2. **Score paired, per trajectory, governed vs ungoverned.** Report the
   discordant counts: how many trajectories the gates flip from fail to pass,
   and, critically, **how many they flip from pass to fail**. An aggregate
   accuracy delta hides the second number, and the second number is the one
   that tells you a gate is over-firing. A gate that wins 400 and loses 40 is a
   different object from one that wins 360 and loses 0.

3. **Report counts, not p-values, for author-designed fixtures.** The scenarios
   are ours and deterministic, so a significance test over them restates the
   scenario count we chose and dresses it as evidence. Counts, seeds, and the
   generator are the honest artifact. (Significance stays appropriate where the
   corpus is external, LoCoMo and LongMemEval, and those harnesses already do it.)

The existing `validate_forbidden_metadata_ignored` check generalizes and should
be kept: fixtures whose poison rows carry *trusted-looking* metadata (high
declared confidence, operator-shaped source tag, high retrieval frequency) are
the only ones that test metadata-spoof resilience, and the harness should keep
refusing to grade a fixture that forgot to lie convincingly.

With §1 in place, each scenario also asserts **which gate** produced the
outcome, by reason code, so a benchmark that passes for the wrong reason
(a poison excluded by a budget truncation rather than by the trust gate) fails.

### §3 An attack-family taxonomy, including the family we cannot defend (Measure, Constrain-Verify)

Generate scenarios per named family rather than as one blended fixture, and
report families separately so the headline mixture stays stable when a family
is added:

| Family | Shape | Expectation |
| --- | --- | --- |
| `benign` | ordinary facts, no gate should fire | gates must not interfere |
| `poisoning` | conflicting value on a lower-trust channel | resolve to the trusted value |
| `injection` | instruction-shaped content stored as memory | quarantine at write, never serve as instruction |
| `erasure` | forgotten attribute queried later; a sibling attribute queried too | refuse the first, **still serve the second** |
| `scope` | two subjects sharing a surface form, distinguished only structurally | never cross |
| `trigger` | conditional backdoor: poison ranks top **only** when a rare token appears in the query, dormant otherwise | defended by provenance, not by spotting the token |
| `same_channel` | poison arrives on the **same channel at the same trust**, written later | **known boundary, expected to fail** |

Two families deserve argument.

**`trigger`** is the family a label-based defense silently fails. The poison is
written first and carries a rare token; on ordinary queries recency favors the
true fact and the memory looks clean, so a benchmark of ordinary queries reports
a healthy system. It surfaces only on the triggered query. A defense that works
here must key off *provenance*, not off any property of the text, which is
exactly the argument for the tier proposal, and this family is the test that
proves it rather than asserting it.

**`same_channel` is the honest boundary, and it directly amends the tier
proposal's stated scope.** If the poison arrives through the same fully-trusted
channel as the legitimate fact, at equal trust, written later, then a
provenance-tier model has **zero signal**: it is indistinguishable from the user
correcting themselves, and latest-wins supersession serves it. Tier 1 does not
mean "true"; it means "a human asserted it", and a human channel can be wrong or
compromised. This should be written into the tier proposal as a named
non-goal before it is accepted, and measured here as a family expected to fail,
rather than discovered later as a hole in a shipped defense.

There is a partial answer worth building because the substrate already holds the
data: **source-independent corroboration**. A value asserted by two *independent*
sources outvotes a single fresh assertion at equal trust. Aimee has one-row-per-
assertion lineage (`memory_lineage_insert`) and a `memory_provenance` table; what
it lacks is a resolver that counts *distinct* `source_ref`s for a
(subject, relation, value) rather than counting vector units of one memory
(§0, fourth gap). Scoped as an **opt-in conflict-resolution mode**, measured on
a `same_channel_corroborated` family, and honest that the equal-trust 1-vs-1
case stays unsolved.

### §4 Two-phase recall: candidate metadata, then release (Enforce)

Split recall into a phase that returns **no content**:

- **Phase 1, candidates:** ids, scores, provenance, and per-candidate verdict
  and reason (§1 is already computing exactly this). The agent can see *that*
  three sensitive facts exist and were withheld, without a byte of their text
  entering the context window.
- **Phase 2, release:** hand out content for named ids, **re-evaluating the
  gates at that moment**.

The re-evaluation is the substance, not the ceremony. A phase-1 eligibility is a
statement about the past; if an erasure, a rejection, a sensitivity change, or a
retention expiry lands between the phases, a stale eligibility is worthless by
design. Phase 2 is also the natural place to emit the serve-side WORM record
from §1, because it is where content actually crosses the boundary.

`emit_candidate` and the section/budget loop around it already form the seam:
today they select and serialize in one pass; the split makes selection
inspectable and serialization gated. Existing callers keep a one-shot
convenience wrapper that runs both phases, so this is an added capability, not a
migration.

### §5 Evidence re-authorization at the action boundary (Enforce)

`policy_check_tool` authorizes the action. Add a companion question at the same
boundary:

> The plan rested on memory *M*. Is *M* still servable, **now**?

Answering it is nearly free once §1 and §4 exist: it is the same gate evaluation
against the same reason vocabulary, keyed by memory id, returning
`still_valid` plus a reason. The failure it prevents is real and current:
a long-running or resumed task plans against a fact, the user forgets or rejects
it mid-run, and the side effect fires on retracted evidence. `pre_tool_check`
already consumes a policy decision; this adds a second, evidence-shaped input to
the same enforcement point rather than a new one.

Scope it honestly: this gates **acting on** a memory the agent already holds. It
does not, and cannot, unwind a value the model has already internalized into its
reasoning earlier in the turn. It is a re-authorization hook, not amnesia.

### §6 Erasure as durable state, not a delete (Enforce)

Keep the destructive branch of `memory_delete_as`: it should still physically
remove the row, its vector points, and its provenance. Add, in a store
deliberately **separate from the memory graph**, a durable tombstone recording what was erased (normalized term tokens
plus the raw surface form), its scope, the requester, and the time.

Retirement is not a substitute. It versions the row and keeps the content
readable through `memory_fact_history`, which is correct for a model correction
and is the opposite of what erasure means. Three properties neither branch
provides today:

1. **Restore safety.** Restoring a snapshot taken before an erasure silently
   resurrects the content. This got harder, not easier: the `aimee data db`
   backup/check/recover group was removed when the store became PostgreSQL
   (`cmd_data.c:534`), so restores now happen entirely outside Aimee, through
   `pg_restore`, with no hook for the process to notice. Because tombstones live
   outside the graph, the supported repair is: restore, then re-union the
   tombstone set. Idempotent, audited, and it reports how many entries it
   re-applied.
2. **Write-side erasure.** A re-ingest of an erased value is quarantined, not
   stored as a fresh clean record. Without this, "forget X" survives exactly
   until the next time X is mentioned.
3. **A recall reason.** `erased` becomes a §1 reason code, so an erasure that
   is enforced is *visible* as enforced, not merely as an absence.

Matching stays token-based by default. Paraphrase-level matching ("wants kids"
erased; a note says "planning a family") requires an embedding provider, so it
is **opt-in, thresholded, and fails audibly** when the provider is down: never
silently fails open, and never silently fails closed either. It catches
similarity, not inference, and the proposal should say so.

An **erasure receipt** (what was targeted, what was removed, from which stores,
requested by whom, appended to the WORM ledger) is the artifact that makes
`aimee memory op forget` auditable. It is evidence of the action taken, not a
claim that no derived copy exists anywhere.

### §7 Channel sensitivity: sensitive by provenance, not only by content (Classify-Score)

`memory_pii_rel_sensitivity` keys sensitivity off the `rel_type`, a property of
*what the fact is*. Add an orthogonal axis: a property of *where it arrived*.

The motivating case is a human free-text field: a note, a scratch comment, a
pasted fragment. Nothing in it need look sensitive token-by-token, and a
content-keyed classifier passes it, yet free-text is precisely where volunteered
sensitive material lands. The channel is the signal, and it is available at
ingest with no classification at all.

Concretely: the write path records a channel sensitivity alongside the existing
tier and trust, and a high-sensitivity channel holds content for explicit
release rather than admitting it at the sensitivity its `rel_type` implies. This
is a small amendment to the tier proposal's classification entrypoint (it is the
same write-side hook, carrying one more field), and it composes with the PII
gate rather than replacing it: `max(content_sensitivity, channel_sensitivity)`
decides.

Held records need a release path (an explicit, authenticated confirmation that
promotes them), and the release is a §1-recordable event. Without the release
path this is a data black hole, so the two ship together.

### §8 A memory invariants file, and fuzz over semantic outcomes (Constrain-Verify)

A short, blunt document listing what must be true regardless of what the score
says, under one governing rule:

> **A change that improves benchmark recall while violating one of these is a
> regression.**

The initial list, all of which are already true or become true above: erased
memory is never selected; rejected memory is never selected; injection-shaped
memory is never treated as an instruction; wrong-scope memory is excluded
*before* ranking can make it attractive; superseded facts do not resurrect as
current truth; every selected memory has provenance; every excluded memory has a
reason; a persistence reload does not change retrieval results; replaying the
same ingest sequence does not create duplicate active truth; adding irrelevant
distractors never makes an excluded memory selectable.

Back it with **seeded fuzz over operation sequences** (writes, supersessions,
rejections, erasures, scope collisions, injection-shaped content, snapshot
round-trips), comparing *semantic* outcomes across seeds: which claims were
selected, whether the turn abstained, and **which reason codes fired**. Not row
ids, which are expected to differ. The reason codes from §1 are what make this
possible: without them the only observable is the final string, and a fuzz test
over final strings is a change detector, not an invariant test.

### §9 A claim register for memory and governance claims (Measure)

Two documents, both cheap, both aimed at the same failure: a number that
outlives the conditions that produced it:

- **A claim register**: every memory/governance claim we make, with its
  evidence level, the artifact that backs it, and its risk if wrong. It makes
  the distinction between "measured on an external corpus", "measured on a
  fixture we wrote", and "asserted from the design" a *field*, not a matter of
  how carefully someone read the prose.
- **A measurement changelog**: when a re-measurement changes a published
  number, record the before, the after, and the cause, **especially when the
  correction moves against us**. A harness bug that understated a baseline and
  was quietly fixed is indistinguishable, from the outside, from one that was
  never found.

`benchmarks/locomo/BENCHMARK_RESULTS.md` already practices the hardest part of
this: it names an external anchor, tabulates every way our harness deviates
from it, and requires deviations beyond a threshold to be explained. Generalize
that discipline; the register is the index that makes it enforceable rather
than per-benchmark habit.

One rule worth adopting explicitly, because cross-harness comparison is the
standing temptation: **rank only what was measured in one harness.** Numbers
from other harnesses (third-party evaluations, vendor self-reports) belong in
separate tables, quoted with their conditions, never merged into one leaderboard.

---

## Non-goals

- **A separate compliance or data-subject-request subsystem.** §6 and §7 exist
  because a memory that cannot durably forget is *incorrect*, not to serve a
  regulatory workflow. No request intake, ticketing, or approval routing is
  proposed here.
- **A second audit store.** §1 and §6 add producers to the existing WORM ledger.
- **A second policy engine.** §5 adds an input to `policy_check_tool`'s
  enforcement point; it does not create a parallel decision path.
- **Changing retrieval ranking.** Every gate here filters. None improves recall,
  and none should be justified by a recall number.
- **Free-text reason strings.** The vocabulary is closed and versioned, or it is
  not testable.
- **Defending `same_channel`.** Named, measured, expected to fail (§3).

## Sequencing

§1 is the prerequisite for §2, §4, §5 and §8 and should land alone, because it
is also the largest standalone win: it makes the existing gates observable
without changing a single decision any of them makes. §3 and §7 should be folded
into the provenance-tier proposal **before** it is accepted, since both change
its stated scope. §2, §6, §8 and §9 are independent thereafter; §4 and §5 are
the largest and should follow the benchmark, so their effect is measurable when
they land rather than argued.

## Open questions

1. **Reason-code granularity.** One code per gate, or a code plus a
   gate-specific detail field? Coarse codes are stable test targets; detail is
   what an operator actually wants at 2am. Probably `code` + optional bounded
   `detail`, with only `code` assertable in tests.
2. **Turn-local trace budget.** What is the per-turn record cap before eviction,
   and does an evicted record leave a counter? (It must, or the trace is
   quietly lying about completeness.)
3. **Does the serve-side WORM record belong in the same chain as governed
   actions,** or a separate stream with its own checkpoint cadence? Recall
   volume is orders of magnitude above action volume.
4. **Where does the tombstone store live** relative to the DB1/DB2 split, given
   that erasure must outlive a restore of either?
5. **Independence, for corroboration (§3).** Two lineage rows with distinct
   `source_ref`s are not automatically independent: two ingests of the same
   upstream document are one source wearing two hats. What is the minimum
   defensible independence test, and is it worth building before the family is
   measured?
6. **Does channel sensitivity (§7) need its own vocabulary,** or does it reuse
   `rel_sensitivity_t`? Reuse is tempting and probably wrong: the two axes
   answer different questions and will drift.
