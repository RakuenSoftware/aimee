# MR-09 implementation and validation — 2026-09-26

Status: implementation validation in progress; this is not a promotion record.

## Serving contracts

Independent eligible lexical, dense and graph pools survive until native fusion.
A full 128-row lexical/PageRank input reserves up to 64 distinct graph neighbors
within the existing 128-node and 8192-link work bounds. The outer caller limit
still applies. Cross-arm owner/revision conflicts are excluded with a trace.
Request-local capability observations distinguish unsupported, unavailable,
not-executed and executed arms, preserve mixed nested outcomes and report bounded
fallback. In particular, the native KB recall bundle does not advertise dense
retrieval; code seeding is a structural projection, not an independent score vote.

The versioned baseline artifact is
[baseline-policy.json](../../tests/eval/memory_mr09/baseline-policy.json).
Native optional PageRank ordering moves a record at most three positions within
its existing scope stratum, with deterministic base-order ties. The assertion
lexical stage individually caps confidence and authority at 0.0625, jointly
0.125 on its 1/3.5/4 query-class scale. The graph relevance stage individually
caps observed-link, utility and confidence adjustments at 0.025/3, jointly 0.025
on its relation/hop scale. A score-stage base gap greater than twice that stage's
joint bound cannot be reversed by its priors. These are separate native stage
contracts, not an additive cross-stage probability or a fitted quality claim.
Actual contributions, input validity, base/final positions and policy identities
are inspectable; unsupported or nonfinite evidence does not become a score.

## Optional final selection

The owner-only `AIMEE_MEMORY_SELECTION_POLICY=typed-diversity-rankwindow3-v2`
selects the frozen
[diversity-v2-policy.json](../../tests/eval/memory_mr09/diversity-v2-policy.json).
Any other value uses baseline selection. This applies to typed context, not a
claim that every legacy prompt channel has the same diversity policy.

The selector gathers at most 128 candidates, retains at most 64 items, reserves
mandatory constraints/corrections first, then strongest required evidence, then
one desired item per enabled type. Duplicate mandatory copies do not create
thirty separate reservations. Discretionary text/family diversity operates only
within a three-position, same-channel base-rank window. Families require exact
revision matches in complete canonical ancestry; they never certify independence.
For an independence obligation, v2 also protects bounded representatives of distinct
complete origins behind duplicate claims (up to the requested count, minimum two,
maximum sixteen). This is conservative preservation of potential support, never
an independence verdict. The earlier v1 artifact remains frozen for audit.
Automatic serving counts never increase authority or confidence, and exposure
adaptation remains disabled. The canonical PostgreSQL lineage fixture verifies
thirty copies share one origin, a separate root keeps its distinct origin, and
both representatives survive the byte cap while independence stays unavailable.

Channel/total estimates and exact rendered-byte limits still win over reservations.
Whole-row pruning removes discretionary tails before protected tails, reports
unsatisfied floors, and recomputes coverage. Priority commitments survive validated
outer repacking. Hard provider-token counting remains explicitly unavailable;
the preexisting hard-token refusal is preserved. No zero or tiny budget can be
expanded to satisfy a floor.

Atomic experiment rollback is the owner configuration change back to the frozen
baseline (unset the selector variable). A request snapshots one compiled artifact;
repository text and caller arguments cannot tune it. The baseline retains hard
eligibility, fair pools, source versions and truthful diagnostics.

## Evidence collected so far

- Prior-cap restricted-role PostgreSQL replay: passed, 279.130 seconds.
- Full local memory race suite after capability envelope updates: passed, 15.849 seconds.
- Selector tests: thirty duplicate candidates before required evidence; constraints
  and authoritative corrections; exact byte limits including zero; outer repacking;
  type/item reservations; disabled parity; deterministic nearby diversity.
- Randomized rank-cap test: 1,000 permutations per configured cap, including 128 rows.
- Native score tests: individual/joint caps, maximum-overturn property, invalid
  input reporting and rejection of forged imported health proofs.
- Repository lint: all 77 checks passed.

The final full PostgreSQL memory race suite passed in 343.113 seconds, including
the actual owner selector retaining the required rank-31 hit in 1,507 rendered
bytes behind thirty copies. Dense unavailability correctly keeps coverage unknown;
the test does not reinterpret retained lexical evidence as a complete hybrid read.
The exported memory owner build passed in 5.574 seconds and native ingress passed.
Compact dispatch-receipt parts retain the selection policy/version digest and
missing-type list across native refreshes. Outer-repacking regression checks pass with protected selection commitments and
only the originally enabled channels contributing desired floors.

The frozen controlled code-navigation pilot used six fit tasks and six separate
held-out tasks from commit `60022abc1`. All 12 real model calls were valid, both
arms answered 6/6 held-out tasks correctly, and fitting selected all arms with
zero exposure penalty. Estimated cost was $0.011953 per arm under the explicitly
hypothetical $1/$1 per-million input/output token model (cached input charged at
the same rate); this is not actual billing or a vendor price claim. p95 was
8.024 seconds baseline and 7.846 seconds fitted; with six observations it is the
sample maximum. Arm/rank/serving-count features were controlled, not native
observations. The pilot found no strict fit improvement and is explicitly
**ineligible for promotion**. It does not establish production quality or permit
learned routing/exposure. See [raw pilot results](memory-mr09-evidence-2026-09-26/routing-pilot.json).

Isolated deployment and its functional/overhead checks are still being collected.
MR-09 is not closed by this checkpoint. CT100 production remains outside the
candidate rollout.
