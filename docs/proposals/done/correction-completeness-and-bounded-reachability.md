# Proposal: Correction completeness and bounded reachability in a durable memory substrate

- **State:** DONE — §3.1 through §3.10 implemented and validated 2026-08-25.
- **Date:** 2026-08-25.
- **Charter roles:** Constrain-Verify / Gate-Promote / Enforce.
- **Thesis:** A memory substrate that already carries bi-temporal validity, an explicit
  trust-state machine, an append-only mutation audit and a governed write gateway has built
  the machinery of correction but has not proved that the machinery holds. Close that proof,
  then stop three reachability signals from leaking into belief. Add no new authority, no new
  approval vocabulary, no second store and no new service.

## 1. Problem

Eleven defects share one root. In each, a mechanism *designed* to be complete is complete only
over a prefix, a literal, or a single lane — and nothing asserts otherwise. Failure in this
class always looks like success: the query returns, the delete reports rows changed, the
synthesized record carries high confidence.

### 1.1 Reclamation stops at a buffer

Deleting a record reclaims its derived vector points by listing child units into a fixed
stack array and iterating. The listing helper fills to capacity and returns, with no cursor
and no signal that rows remain. Any record with more children than the buffer holds leaves the
remainder as live rows in the vector table. That table is keyed on a synthetic point
identifier and carries no foreign key back to the record, so the cascade that reclaims child
rows never reaches it. Orphans retain their payload and stay in the ANN index.

A sweep for the shape — a fixed stack buffer passed as the bound to a list helper that fills
to capacity and returns — finds three correctness-relevant sites: two reclamation paths with
different buffer sizes, and one safety check (§1.9). The remaining matches are legitimately
bounded batches (search, ranking, maintenance) where truncation is intended.

### 1.2 No negative retrieval assertion

The substrate has suppression, invalidation, quarantine, erasure, scope filtering and
lifecycle-filtered views. Nothing asserts that any of it survives contact with the *read
path*; the closest existing assertion counts rows at the mutation layer. This matters more
here than in a single-lane system because recall is plural — lexical, dense, graph expansion,
episode cards, derived profile views, envelope assembly — and a value suppressed in one lane
can re-enter through another.

The test discipline matters as much as the tests. A harness that reimplements its caller
certifies the storage layer and takes the wiring on faith, and the wiring is where these bugs
live. Every assertion here must drive the production path.

### 1.3 The tombstone is keyed on literal text

Rejection semantics are otherwise sound: the exact-match lookup deliberately does *not* filter
by lifecycle, so a re-assertion finds the dead row, and revival is gated on actor authority
rank. A lower-authority writer cannot revive what a higher-authority actor invalidated. The
hole is the key. Identity is the raw triple, and no normalization occurs anywhere in the
mutation seam — no case folding, no Unicode canonicalization, no whitespace collapse, no alias
resolution — despite canonical entity infrastructure existing elsewhere. Two spellings of one
fact are two facts. The realistic trigger is not an adversary; it is the extractor emitting
the same claim with different surface form on the next pass.

### 1.4 Derived writes bypass rejection

Summarisation, consolidation and cross-session synthesis read source records and emit new
ones. None consults the lifecycle of the sources it derived from. A rejected claim still
present as raw evidence can be re-manufactured as a fresh derived record carrying none of the
rejection. Lineage rows already exist and are already written; they are simply never read.

### 1.5 Frequency is converted into confidence

Cross-session pattern synthesis assigns confidence from session count alone, scaling to near
certainty purely on how often a claim recurred, with a comment naming cross-session frequency
as the strongest available signal. That is popularity presented as truth, and it self-feeds: a
higher-confidence record ranks higher, is injected more, is restated more, recurs in more
sessions.

The substrate is otherwise careful here — confidence classes are provenance-keyed and set by
authority, durable promotion is gated on independent *evidence* count rather than retrieval
count, and the reachability score decays on its own clock and is clamped. This one path is the
exception that undoes the discipline of the rest. There is also no provenance ceiling: nothing
caps how high a record may climb based on where it came from, so a popular error can become
permanent canon.

### 1.6 A derived-record background job ignores scope entirely

The same synthesis query selects candidates across the whole store with no workspace, project,
user or tenant predicate, grouping on a distinct-session count. A claim appearing in three
sessions of three unrelated projects is promoted into the top tier and becomes globally
reachable. Scope is enforced on the read path but omitted from this background job and from
the derived rows it writes — the precise failure mode where scope is treated as a retrieval
filter rather than part of identity. Combined with §1.5, cross-context recurrence is laundered
into high-confidence global canon.

### 1.7 Recall is unconditional

The read stage gates on a non-empty query, a token floor, and a global toggle. There is no
per-turn relevance decision, so an acknowledgement pays full retrieval cost and receives an
evidence envelope that reads as authoritative. The cost that matters is not latency; it is
that irrelevant evidence injected into an unrelated turn bends the answer, and the bent answer
feeds the improvement loop.

### 1.8 Assembly has no origin quota

The envelope builder sorts candidates by score under a byte budget with no cap on how many may
share one origin. Deliberate chunking sharpens this: one record splits into many separately
embedded units, and adjacent units of one source score near-identically against one query. The
envelope looks full and well-ranked while being several slices of one source, having displaced
the one item from elsewhere that would have changed the answer.

### 1.9 Contradiction resolution never reaches the write path

Contradictions are detected, logged, linked, given an epistemic directive and exposed through a
resolve verb — a real disposition, not merely a flag. But detection reads only current content
for contradiction shape; it never consults whether *this pair* was already resolved. A
resolution invisible to the write path is undone by the next extraction pass, so the same
contradiction reappears, the queue refills, and the detector's precision becomes unmeasurable.
The detection scan is additionally bounded by a fixed buffer (§1.1), so a heavily-revised key
silently stops being checked at all.

### 1.10 No activation state between turns

Every turn's recall is computed from scratch, so a unit is above threshold or absent with
nothing in between. It repeats every turn while the topic holds, then vanishes the moment
phrasing drifts. There is no per-unit state anywhere in the read path.

### 1.11 Write-to-readable lag is unmeasured

Capture is synchronous; embedding, enrichment and graph projection are queued. The queue table
records enough to derive the interval between a record becoming durable and becoming
retrievable, but nothing surfaces it. This is the property a user notices first — "it forgot
what I just said" — and the one property no comparable reviewed system measures at all.

## 2. Existing machinery to reuse

| Concern | Existing seam | Change here |
| --- | --- | --- |
| Child-unit enumeration | fixed-buffer listing helper | Set-based reclamation; cursor form for real iteration |
| Vector reclamation | per-point delete by synthetic id | One statement keyed on the parent record |
| Trust states | whitelisted lifecycle transition machine | None |
| Bi-temporal columns | valid-time and transaction-time already distinct | None |
| Mutation audit | commit + structured per-field change rows | Record new refusal reasons as ordinary changes |
| Write gateway | central mutation seam + database-level guard trigger | Consult normalized key and resolutions inside it |
| Exact-value lookup | literal triple match | Match normalized key alongside literal |
| Relation normalization | existing relation-name normalizer | Extend the discipline to subject and object |
| Lineage | lineage insert/get written on derived records | Consult before derived writes |
| Confidence classes | provenance-keyed, set by authority | Stop letting frequency set confidence |
| Durable promotion | gated on independent evidence count | None — corroboration, not reinforcement |
| Reachability score | separate column, decayed, clamped | None — already the right shape |
| Scope filters | scope tables and typed filters | Apply them in background/derived jobs too |
| Conflict disposition | record / log / list / resolve verbs | Consult resolutions before re-raising |
| Read stage | query presence + token floor + global toggle | Cheap, fail-open, logged relevance gate |
| Envelope assembly | score-ordered emit under a byte budget | Two-pass selection with a per-origin quota |
| Explain surface | assembly already has an explain variant | Report what quota and gate displaced |
| Async index bookkeeping | queue rows with enqueue and index timestamps | Derive and surface a lag summary |

## 3. Exact behavior

### 3.1 Exhaustive reclamation

Replace fixed-buffer enumeration in every reclamation path with a set-based delete in one
statement scoped by the parent record, so no buffer bounds correctness. Retain a cursor form of
the listing helper for callers that genuinely iterate, reporting whether more rows remain
rather than silently returning a full buffer.

All three sites found by the sweep are repaired. A larger buffer is not a fix, and the smallest
one is not the only one.

The invariant: reclamation must never depend on a compile-time bound over a runtime-sized set.
Where a batch bound is genuinely wanted — a maintenance sweep that should not hold a long
transaction — the bound is legitimate but must be **logged when it truncates**, so a partial
pass is never mistaken for a complete one.

### 3.2 Normalized rejection key

Add a normalized identity alongside the literal triple, computed in the mutation seam:

- Unicode canonicalization (NFKC) first, so visually identical forms converge.
- Case folding, whitespace collapse, trimming.
- The existing relation normalizer on the predicate.
- Retained as text, not only a digest, so audit stays legible; a digest form may be layered
  later where the value itself is sensitive.

Two failure modes observed elsewhere are designed against explicitly:

- **Normalization must be codepoint-preserving.** An implementation that keeps only ASCII
  alphanumerics reduces non-Latin values to the empty string, and every such fact then shares
  one key. It folds case and width; it does not filter character classes.
- **No minimum length below which normalization is skipped.** Short values — dates, versions,
  identifiers, region names — are exactly the values corrected most often.

The exact-value lookup matches the normalized key. The functional-relation incumbent scan
matches it too; otherwise a rephrased incumbent escapes supersession and two spellings of one
functional fact coexist as current. The literal remains stored and displayed unchanged:
normalization decides *identity*, never presentation.

Per the gateway's check order, normalization is step one and the tombstone check precedes the
create/corroborate/supersede decision — not after it.

### 3.3 Derived-write lineage suppression

Before a derived record is written, walk its declared sources through the existing lineage
relation and refuse if any source is rejected or suppressed. The walk is recursive with a
visited set and a fixed depth cap; hitting either cap suppresses, because failure here falls
closed.

The honest limit, stated rather than hidden: a derived writer that omits its source references
escapes suppression. That is a bound on the mechanism, and it argues for source references
being mandatory on derived writes.

### 3.4 Frequency must not set confidence; provenance sets a ceiling

Cross-session synthesis stops deriving confidence from occurrence count. Recurrence is retained
as what it is — a reachability and salience signal — and recorded as such. Confidence on a
synthesized record is the conservative class the substrate already uses for unproven inference,
and rises only through paths that gate on independent evidence or explicit approval.

A provenance ceiling caps how high a record may climb by where it came from, so a popular error
cannot become permanent canon regardless of how often it recurs.

Stated as an invariant: **exposure does not validate, and time does not validate.** Four
dimensions stay in four fields on four clocks — retrieval strength (reachability), epistemic
confidence (evidence), validity expiry (staleness), retention policy (authorization to delete).
No one number may serve two. Telemetry is not truth: that a record is frequently retrieved
proves it is discoverable, not that it is correct.

### 3.5 Scope in background and derived work

Every background job that selects candidates, and every derived record it writes, carries the
same scope predicate the read path enforces. Recurrence is counted **within** a scope, never
across unrelated ones. An unresolved scope is an error, not a permissive default meaning
"all data" — the defaulting form is how private context leaks into shared state.

### 3.6 Turn-level recall gate

A cheap deterministic decision runs before the expensive read, under three invariants:

- **Much cheaper than what it guards** — a token count, a shape test, a known-entity lookup. A
  gate that costs what the operation costs is the operation with extra steps.
- **Fail open** — a gate that errors performs the work. It must never silently produce
  confident, evidence-free answers.
- **Log every skip with its reason**, on the existing retrieval event, so the skip rate is
  measurable before it is trusted.

Both error directions are measured separately — retrievals wrongly skipped and retrievals
wrongly performed — because they have different costs and one accuracy number hides the worse
one. Nothing is gated that has not first been measured ungated; this slice therefore ships the
gate **observing and logging by default**, with enforcement behind configuration.

### 3.7 Origin-diverse assembly

Two-pass selection under the existing byte budget. Pass one admits the best candidate from each
distinct origin, guaranteeing coverage. Pass two spends the remainder in global score order
under a per-origin cap. Origin identity is the parent record and the originating session, both
already available on candidates.

Depth is genuinely needed sometimes, so the cap is a reserve split rather than a rigid
one-per-origin rule, which destroys local context: part of the budget guarantees breadth, the
remainder goes to globally best evidence regardless of origin. Origin labels are preserved into
the emitted envelope so a reader can tell independent corroboration from one source quoted
repeatedly. The explain variant reports what the quota displaced.

### 3.8 Resolutions consulted before re-raising

Contradiction detection consults prior dispositions for the same pair before recording a new
one. A pair already resolved is not re-raised; a resolution is durable against re-extraction
rather than being overwritten by the next pipeline run. The detection scan is unbounded per
§3.1, so a heavily-revised key does not silently stop being checked.

### 3.9 Write-to-readable lag

Derive, from existing queue bookkeeping, the interval between a record becoming durable and
becoming retrievable, and surface it as percentiles on existing health output. No new table and
no new collection path — the timestamps are already written; only the summary is missing.

### 3.10 Per-unit activation state

Give each unit four independent parameters, evaluated as an ordered gate after relevance:

1. **Sticky (N turns)** — having fired, stays eligible for N turns regardless of match, so a
   rephrase does not drop the thread.
2. **Cooldown (M turns)** — having fired, refuses to fire for M turns. The only mechanism that
   prevents per-turn repetition.
3. **Delay (N turns)** — will not fire until the conversation is N turns old, keeping
   background material from leading.
4. **Suppression** — withheld regardless of match; human-authorable.

Evaluation order is fixed and documented: relevance → delay → cooldown → suppression → inject.
Sticky and cooldown are the hysteresis proper — the condition for staying in is not the
condition for getting in — and their precedence is stated rather than left emergent, because
ambiguity there is the commonest defect in existing implementations.

Two constraints are load-bearing:

- **Activation state persists with the conversation, not in process memory.** State held only in
  a process silently resets its cooldowns on reload and repetition returns with no visible cause.
- **Injection must not feed the usage signal.** What the harness surfaces is not evidence that
  the surfaced thing was useful. Activation is excluded from reachability scoring and every
  reinforcement path, or this feature becomes §1.5 in a new location.

Non-determinism is accepted as inherent: identical conversation state yields different context
depending on history. Fixtures therefore assert *what fires and why*, driving the gate from an
explicit turn counter rather than wall-clock state.

**Implemented.** The effectiveness snapshot is deliberately not reused: injection is not evidence
that an injected item helped. DB1 instead owns two dedicated tables,
`context_activation_turns` and `context_activation_events`. One transactional operation atomically
advances the conversation turn and returns the most recent firing turn for each candidate; a
second writes an injection against that exact positive turn. Reading once per turn rather than
once per candidate keeps the state gate cheaper than retrieval. Concurrent assembles serialize on
the session counter and receive distinct monotonic turns rather than racing onto one inferred
index.

The retrieval unit at this seam is the memory candidate keyed by memory row id. Dense and lexical
chunk lanes collapse to that parent before assembly, so cooldown and sticky state cannot be evaded
by retrieving another chunk of the same record. Sticky, cooldown, delay and human suppression
policies are durable fields on that candidate and are available through the memory CLI.

The hot production path is the native per-turn recall path, not a diagnostic assembler. The
one-user `aimee-server` binds the request's credential-associated `aimee-session-id`, advances and
loads DB1 activation state exactly once, and carries the bounded snapshot on the existing
`memory.recall` request. The shared, many-user `aimee-kb` never opens DB1: it applies the snapshot
to DB2 candidates after relevance, returns `activation_managed` only on DB2 rows it actually
selected, and reports the number held. Back in the server, only those marked final rows are
recorded. User-local DB1 rows merged afterward, reminders and directives are deliberately excluded,
so overlapping numeric ids across the two databases cannot create false DB2 activation events.

Task-aware diagnostic assembly uses the same persisted model. It bypasses the context cache while
activation is enabled, because returning a cached envelope would erase a conversation turn and
repeat its prior firing decisions. Explain mode reuses the production pass's thread-local activation
snapshot; diagnostics therefore report the exact turn the real pass used without advancing the
conversation a second time. Store failure clears that snapshot and fails open.

The shortcut not taken: holding activation in process memory. That is the failure this section
already warns about — cooldowns reset silently on reload and the repetition returns with no visible
cause, which looks exactly like the feature working. The contract work was the point, not an
obstacle to route around.

## 4. Completion record

| Section | Production invariant | Regression evidence |
| --- | --- | --- |
| §3.1 | Parent-keyed set reclamation; explicit cursor for iteration | More than 130 child units are drained; vector and queue rows are absent after production deletion |
| §1.2 | Withheld lifecycle and suppression states stay absent from production reads | Archived and suppressed sentinels are absent from list, search and assembled context while an active control remains visible |
| §3.2 | NFKC, full casefold and whitespace identity in the mutation seam | Unicode/short/non-Latin vectors plus normalized functional supersession and tombstone replay |
| §3.3 | Recursive fail-closed source walk before every derived writer; bounded session folds never summarize a prefix or purge an unread suffix | Shared DAG accepted; rejected ancestor, cycle, depth and edge caps refused; session fold proves complete lineage, rejected-source refusal and over-cap preservation |
| §3.4 | Recurrence affects salience only; stored provenance ceiling bounds updates | Merge and L5 synthesis remain at their respective 0.8 and 0.5 ceilings |
| §3.5 | L5 recurrence groups within resolved scope and derived scope replaces ambient tags | Equal claims in two projects produce two exactly scoped rows; unresolved scope produces none |
| §3.6 | Observe by default, enforce by configuration, fail open, separate error directions | Acknowledgement, failure, mode and outcome-counter fixtures drive the production gate |
| §3.7 | Per-section breadth pass followed by capped depth pass; escaped origin labels | Production envelope contains the independent origin and at most two rows from one session; explain names quota displacement |
| §3.8 | Resolved pairs are order-insensitive and are not re-raised; scan is cursored | Reversed pair remains resolved and a contradiction beyond the old page boundary is found |
| §3.9 | Existing queue timestamps yield lag p50/p95/p99, with explicit no-sample sentinel | Health output fixture verifies the empty-sample contract; schema/API checks cover the surfaced fields |
| §3.10 | Persisted monotonic turns and independent sticky/cooldown/delay/suppression gates on the real server-to-kb recall path | C hysteresis and proxy-wire fixtures cover timing, precedence, bounded snapshots and selected-DB2-only recording; the fresh PostgreSQL live rig proves first-fire/cooldown/re-fire, restart durability and session isolation |

Validation is intentionally split across the production paths named above: the focused C memory
suite, the typed-fact lifecycle suite, the Go runtime-family suite, schema/API parity, DB2 contract
and activation gates, generated-table determinism, and repository line/include-fragment gates.

The final 2026-08-25 validation used `scripts/validate-memory-activation-live.sh` in a fresh
unprivileged Debian 13 container (CT 9083) on the `.252` validation host. It started the real HTTP
server, DB1 store module, KB, DB2 PostgreSQL module and PostgreSQL 17/pgvector stack. Against one
DB2-managed preference with a one-turn cooldown, turns 1/2/3 respectively selected/held/selected
the row; turn 4 still held it after a server restart; a second session independently selected it on
its first turn. Database assertions found monotonic turns, events only for selected rows, and zero
effectiveness-snapshot rows. The rig removed its temporary database, owner role and processes, and
CT 9083 was then destroyed with purge. The full local native suite also ended with `All tests
passed`; production `aimee-server` and `aimee-kb` builds, the Go family suite, schema sync, DB1 client
contract and whitespace checks passed.
