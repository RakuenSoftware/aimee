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

The owner-only `AIMEE_MEMORY_SELECTION_POLICY=typed-diversity-rankwindow3-v1`
selects the frozen
[diversity-policy.json](../../tests/eval/memory_mr09/diversity-policy.json).
Any other value uses baseline selection. This applies to typed context, not a
claim that every legacy prompt channel has the same diversity policy.

The selector gathers at most 128 candidates, retains at most 64 items, reserves
mandatory constraints/corrections first, then strongest required evidence, then
one desired item per enabled type. Duplicate mandatory copies do not create
thirty separate reservations. Discretionary text/family diversity operates only
within a three-position, same-channel base-rank window. Families require exact
revision matches in complete canonical ancestry; they never certify independence.
Automatic serving counts never increase authority or confidence, and exposure
adaptation remains disabled.

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

Database-backed selector replay, exported/native owner checks, isolated deployment
and held-out routing/exposure evaluation are still being collected. MR-09 is not
closed by this checkpoint. CT100 production remains outside the candidate rollout.
