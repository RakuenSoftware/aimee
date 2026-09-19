# MR-01: Unified retrieval eligibility and validity

- **State:** In progress; shared current-state retrieval predicates implemented
- **Priority:** P0: correctness foundation
- **Owner:** Go memory module, with PostgreSQL storage and authenticated transport integration
- **Depends on:** None; use the fixture harness in [MR-18](memory-reliability-18-evaluation-parity-and-release-gates.md) from the first change
- **Delivery:** Three reviewable implementation slices

## Problem and intended result

Memory search, visible search, bundle recall, fact recall and graph expansion currently apply different combinations of suppression, lifecycle and time checks. A record can be relevant to a query while being invalid for its requested time or unauthorized for its caller. A ranking score must never repair an eligibility failure.

Provide one versioned eligibility decision for every memory-bearing surface. Personal and shared stores retain their existing ownership; each implements the same behavioral contract over its own schema. Current-state, historical and explicitly diagnostic reads have separate, declared semantics.

## Existing integration points

Implement the shared eligibility decision in `server-go/modules/memory` and use it from `{data.go,visibility_search.go,retrieval.go,fact_recall.go,fusion.go}`. Migrate memory eligibility decisions from the typed context backend `src/kb/db2_adapters/kb_service_backend_context.c` behind that Go contract; memory-specific callers and framing also move to Go under G0. Reuse `memory_row_scope_visible`, transaction-local request scope and existing semantic-assertion filters in `src/modules/db2/c/schema.sql` through the storage owner. A memory-row policy does not automatically protect fact edges, aliases, derived rows or cached projections; inventory those paths explicitly.

## Implemented foundation

The Go `current-validity-v4` predicate now applies active lifecycle, explicit
suppression and half-open valid time before lexical, active-version whole-record
semantic, unit/temporal semantic, graph/PageRank parent, compatibility-window,
recall-bundle, activation and briefing limits. Pending commitments use the same
valid-time gate with their pending lifecycle. Sticky activation does not override
validity, and briefing activity/entity aggregates require current parents. Memory-backed graph evidence uses the same predicate; semantic edge time
bounds use the same timestamp adapter. Mixed graph evidence requires every memory
source to resolve inside the current request audience, including exact-scope
queries; visible evidence cannot admit a hidden or expired dependency. UTC wall timestamps and offset-bearing
forms normalize to instants, using the storage transaction's captured clock.
Malformed or relative/infinite governed times refuse retrieval rather than becoming open
endpoints. Corpus baseline policy fingerprints include this eligibility version.

Restricted-role replay covers current/future/expired/suppressed and excluded
lifecycle states, scope isolation, exact boundary instants, non-UTC sessions,
malformed timestamps, and recovery. Dense/unit and PageRank fixtures include
otherwise perfectly matching future/expired sources. Bundle, activation and
briefing fixtures prove backfill with high-ranked invalid rows exceeding section
limits. Directive/reminder match and briefing views now share normalized expiry;
sweeps use the same exact upper boundary. PostgreSQL replay covers offsets,
pre-limit exclusion, malformed timestamps and recovery. Operator lists preserve
unswept lifecycle state. Assertion world-valid and belief-time SQL now preserves
stored offsets and fractions and uses the same inclusive-start/exclusive-end
semantics, with non-UTC replay coverage. The public anchor format remains
second-precision UTC. This does not complete the host privilege vocabulary,
fact/cache surfaces, historical/belief-time
modes, final-release generation checks or the validity command below.

## Contract

Add an internal `EligibilityContext` supplied by the authenticated host: principal reference, authorized audience, workspace/project, purpose, query mode, valid-at time, believed-at time, policy version and revocation generation. Model text cannot supply principal authority or grant `include_all`.

Return an `EligibilityDecision` with `eligible`, `reason_codes`, `lifecycle`, `temporal_applicability`, `evidence_state`, `authority_class` and the checked record version. The user-facing validity projection may display `valid`, `historical`, `stale`, `superseded`, `expired`, `provisional`, `quarantined` or `unknown`; retain the underlying dimensions rather than compressing every failure into one scalar multiplier.

Apply rules in this order:

1. Authenticate scope and purpose. Filter unauthorized candidates before ranking or metadata exposure.
2. Enforce deletion, rejection, quarantine and explicit suppression policy. A historical request does not bypass authorization, erasure or quarantine.
3. Evaluate lifecycle and half-open time intervals: `from <= requested_time < until`, with explicit handling of open endpoints. World-time and belief-time remain independent.
4. Check required evidence availability and contradiction state. Unknown evidence is explicit; it is not silently treated as validated.
5. Apply the ordinary-serving horizon from [MR-10](memory-reliability-10-deterministic-utility-horizons.md) when enabled for this record category and request purpose.
6. Recheck after graph expansion and at context release. Every traversed node/edge and returned target must satisfy the applicable policy.

Capture the request clock once. Normalize legacy timestamp representations at the storage adapter; reject malformed governed-time values rather than comparing inconsistent text formats. Diagnostic access to excluded records requires its own authorized purpose and must return no more metadata than that purpose permits.

## Implementation slices

**1. Contract and storage predicates.** Implement the shared types, reason vocabulary and SQL predicate builders or views. Bind scope to transactions under non-owner runtime roles. Pin `include_all` to a privileged capability instead of trusting a request boolean.

**2. Serving parity.** Route search, visible search, bundles, facts, previews, typed channels and graph legs through the Go memory decision, in both supported placements. Convert confirmed C callers and remove their duplicate memory predicates when the corresponding Go operation lands. Make legacy activation/workspace parameters effective or reject/deprecate them explicitly. Return `unsupported_mode` where an adapter cannot provide believed-at reconstruction.

**3. Release and diagnostics.** Add `aimee memory validity <id> --mode current|historical` as a projection of the real serving decision. Join final release to the checked record version and revocation generation; [MR-16](memory-reliability-16-evidence-bound-actions-and-composition.md) defines action-time use.

## Acceptance gates

- One fixture includes current, future, expired, suppressed, superseded, archived, quarantined, deleted, revoked and cross-scope records. All advertised endpoints return the expected eligible set.
- Historical recall returns an authorized old version at the requested time while excluding erased and unauthorized content.
- Graph traversal cannot expose a permitted target through an unauthorized intermediate node or reveal that node's identity.
- Concurrent pooled requests cannot inherit each other's transaction scope. Owner/superuser tests cannot substitute for the non-owner runtime test.
- An edit or revocation after candidate retrieval invalidates release or forces a new decision; the earlier decision is retained as history.
- Pure lexical, dense-only and graph-only candidates receive the same hard gates.
- Actual Server and KB memory processes return equivalent domain decisions for equivalent authorized fixtures. A disconnected module cannot activate a C eligibility fallback; boundary checks cover migrated typed adapters.

## Rollout and rollback

Compare old/new decisions on authorized fixtures and sampled shadow requests. Ship confirmed eligibility corrections independently of ranking experiments. After enforcement is enabled for a surface, a rollback may restore the previous ranking policy but must preserve the corrected access/lifecycle gates. Measure exclusions by reason, false exclusions on historical fixtures and query latency.

## Boundaries

This proposal does not change canonical taxonomy, turn confidence into probability or make all stores share one database. No numeric relevance boost can override an ineligible decision.

[Program and common contracts](memory-reliability-00-program.md) · [Requirements coverage](memory-reliability-requirements-coverage.md)
