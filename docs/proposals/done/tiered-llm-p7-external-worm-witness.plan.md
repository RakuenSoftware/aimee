# P7 external WORM witness and full kill matrix

- **State:** implemented and validated on branch `rewrite/go-server-wfe` (through
  `ecbfbc35`); merged to `testing` via PR #1930 (2026-07-24). E1, E2, and E3 are all delivered — the
  P2b release gate flip is live (`kb_egress_release_allowed()` returns
  `witness_release_gate_open()`). Validated end-to-end on a fresh CT103 env: witness
  unit suite, integration gate (producer/emit/tamper/recovery/canary), the E3 kill
  matrix, hardened boot over verify-full TLS (both directions), runtime-role least
  privilege, and a ThreadSanitizer lane for the release-gate cross-thread cell. A
  clean roundtable review approved the core diff.
- **Depends on:** P7-reseal D1, D2a, D2b, D3a, D3b (merged), P3a WORM ledger, and
  the existing kb audit chain.
- **Enables:** the last open P7 line item — `external WORM delivery + full kill
  matrix` — and therefore P2b production org egress.

## Why this is the remaining P7 scope

Everything P7 has landed so far makes evidence *correct inside PostgreSQL*. The
kb audit chain (`kb_audit_event`, `src/modules/db2/c/kb_audit_worm.c`) is hash-chained and
trigger-protected; the reseal outbox (`kb_vault_rewrap_worm`) has deterministic
event IDs and exact replay checks; D3b added the atomic primary open event
(`kb_vault_open_event`) with a full row hash.

None of that is durability of evidence. A sufficiently privileged PostgreSQL
actor can rewrite the chain and the outbox together, which is exactly the threat
the hardened-vault proposal names in §6. D3b said so explicitly — neither a
successful local outbox append nor the primary hash chain may be described as
external WORM delivery, and external witnessing remained a later release gate.

That gate is real code today, not a doc note: `kb_egress_release_allowed()` in
`src/kb/kb_vault_policy.c` returns 0 unconditionally outside the conspicuous
`AIMEE_P2B_INTEGRATION_TEST_OVERRIDE` build, and `src/kb/kb_main.c` and
`src/kb/http/kb_http_egress.c` both consult it. This umbrella is what allows that
function to return a real answer.

## Delivery split

The witness touches the admission hot path, adds shared cross-instance state, and
ends by flipping a production release gate. Reviewing that as one change repeats
the mistake the reseal split was created to avoid. Delivery is fail-closed and
ordered:

1. **E1 — witness record, signed checkpoint, export form, and evidence log.**
   Canonical witness record encoding and per-entry evidence digest, the signed
   Merkle checkpoint format with per-shard inclusion proofs, the deterministic
   exported rendering, evidence grouping by admission dispatch key, and the
   durable evidence log plus per-`(tenant, provider)` shard counter rows created
   by DML upsert (no runtime DDL). No admission caller, no emission, no
   production invocation.
2. **E2 — atomic witness append, emission, and continuous verification.** The
   witness append and shard advance committed *in the same transaction* as the
   source event (see the atomicity invariant), checkpoint cadence ("every N
   entries or T seconds, whichever first"), log/OTLP emission of records,
   checkpoints, and inclusion proofs, numeric-only metrics for health and
   backlog, continuous chain verification with typed integrity alerts, the
   offline verifier tool, and boot fail-closed for any key-holding kb whose
   chain verification is off. **E2 does not touch
   `kb_egress_release_allowed()`.**
3. **E3 — full kill matrix, then the release-gate flip.** The exhaustive
   CT103/CT260 restart and signal-level kill matrix across every P7 durable
   boundary — the reseal boundaries D3b enumerated plus the new evidence-append,
   shard-advance, checkpoint, and emission boundaries — with raw-key canary scans
   over database, files, logs, and crash artifacts. Only after that matrix passes
   does E3 make `kb_egress_release_allowed()` return a real answer.

**The gate flip is in E3 on purpose.** An earlier ordering put it at the end of
E2, which contradicted this umbrella's own header: E2 would have opened
production org egress before the kill matrix proved crash and restart behavior at
the boundaries E2 introduced. A test matrix that runs after release is not a
release gate. Branch ordering and prose are not enforcement, so the gate is code
in the slice that earns it.

Each slice gets its own reviewed plan, adversarial branch review, target
validation, and merge:

- E1 — `tiered-llm-p7-witness-e1-record-checkpoint-evidence-log.plan.md`
- E2 — `tiered-llm-p7-witness-e2-append-emission-verification.plan.md`
- E3 — `tiered-llm-p7-witness-e3-kill-matrix-and-release.plan.md`

E1 and E2 were originally split so that the acknowledgement rules could be proven
before any budget depended on them. There is no acknowledgement and no budget
(see below), so that split had nothing left to protect. The compensating control
is that E1 still ships **no caller** — nothing emits, nothing gates, and the
release gate stays closed until E2.

## Witness architecture: tamper-evident chain on aimee, exported everywhere

**Decision:** aimee-kb holds a tamper-evident, hash-chained evidence store and is
the system of record for it. Everything in that store is exported outward
continuously as logs and metrics. There is no third-party append-only sink, no
delivery receipt, no per-consumer watermark, and no confirmation protocol.

The security property has two halves, and they are not equally strong. Stating
them precisely matters more than stating them impressively:

- **Locally inconsistent tampering is detected immediately, unconditionally.** A
  bad link, a regressing sequence, or a checkpoint that fails verification is
  caught by aimee's own continuous verification with no external round-trip and
  no dependency on any collector.
- **Consistent tampering and clean rollback are detectable only by comparison,
  and only for evidence that actually reached a collector before the compromise.**
  A privileged actor who rewrites every local artifact coherently — chain, shard
  heads, Merkle state, checkpoint sequence — leaves nothing locally wrong. What
  exposes it is the contradiction with copies already on hosts the attacker
  cannot reach backwards in time to amend. Evidence that never left the host
  before compromise has no external anchor and is not covered.

That second half is a real limit, not a caveat to bury. No slice may write that
tampering "cannot go unnoticed" without qualification.

Either signal is sufficient. **This umbrella's deliverable is detection and
bounding: proving tampering occurred and establishing when.** Reconstructing what
was actually done is a different mechanism and belongs to the event bus, which is
designed to record and replay activity across services up to the point of a full
takeover. The two compose: the witness chain establishes that the local history
is false and from which point, and the replayable bus stream supplies the true
activity over that window.

No slice may claim the witness export alone rebuilds the original event history —
hashes cannot be inverted, and only the fields actually exported are readable
from it. Records do carry the non-secret identifying fields an operator needs to
act on a detection (request id, principal, `provider:cred`), because §6 requires
them for the audit purpose regardless. But the investigation claim rests on the
bus, and slices must cite it rather than overstating what the chain provides.

**All evidence bytes ride the log/OTLP path. Metrics carry numbers only.**
Records, signed checkpoints, and inclusion proofs are emitted as log/OTLP events.
The Prometheus surface (`GET /v1/metrics`, behind org-admin auth or the
constant-time scrape token — `src/kb/http/kb_http_telemetry.c`,
`src/headers/kb_http_telemetry.h`) carries only numeric health signals: current
checkpoint sequence, evidence count, emission backlog, and verification-failure
counters.

An earlier draft put signed checkpoints on the metrics surface on the theory that
they were "small and constant-shape." That does not survive contact with the
Prometheus data model. Samples are numeric values plus labels: carrying changing
roots, signatures, and sequence numbers as labels mints a new time series per
checkpoint (unbounded cardinality), while packing bytes into numeric samples
risks float64 precision loss. Exposition is also a sampled snapshot, so
checkpoints produced between scrapes would simply be missed — and a missed
checkpoint breaks predecessor-linked verification for every consumer that
retained the ones on either side of it. Metrics are a health surface, not an
evidence transport.

**Why there is no unwitnessed budget.** The hardened-vault §6 backlog bound
existed because evidence sat in a local outbox *waiting* to reach an off-host
sink that was authoritative; the bound capped how many key uses could occur while
their only durable copy was still local. That is not this architecture. Evidence
is durably committed on aimee-kb before the key is used — the existing P7
admission rule — and export is redundancy on top of a copy that already exists.
There is no in-flight-sole-copy window for a budget to bound, and with no
confirmation protocol there is no event that could release a reserved slot, so a
budget would only ever deadlock.

Export lag is therefore an **alerting signal, not an admission gate**. A
collector that is down or slow reduces redundancy; it does not mean a key use
went unaudited, and halting all org egress because a metrics scraper died trades
a real outage for no assurance gain.

**Residual.** An attacker who owns the kb host controls what is exported from
that moment on, and evidence generated after the compromise but before the next
export has no external copy. Detection of the *prior* history still holds,
because those copies are already gone from the host's reach.

## Non-negotiable invariants

- **No single source of authority.** Neither aimee's chain nor any single
  exported copy is authoritative alone. A local chain break proves tampering by
  itself; a locally clean chain that contradicts a retained exported copy proves
  it just as well. No slice may describe a durable local append or the primary
  hash chain as sufficient on its own.
- **Coverage is conditional and must be stated that way.** External detection
  covers exactly the evidence that reached a collector before compromise.
  Evidence that was never emitted, or emitted but never retained, has no external
  anchor. Slices state this limit wherever they state the property.
- **This umbrella delivers detection and bounding; the event bus delivers
  reconstruction.** No slice may claim the witness export alone rebuilds the
  original event history. Claims about determining what was done must cite the
  event-bus record/replay mechanism, not the chain. Within these documents the
  word "reconstruction" is reserved for that mechanism; witness-side grouping
  supports incident *triage*.
- **Emit everything, gate nothing on it.** All evidence is *emitted*; whether a
  downstream service retained it is not observable from aimee and must never be
  claimed. Alerts fire on what is actually measurable locally — emission backlog
  depth and failed OTLP sends — never on "a collector is down," which a design
  with no consumer registry cannot know. There is no delivery receipt, no
  watermark, no consumer registry, and no unwitnessed budget.
- **The witness append is atomic with the source event, on every path.** The
  witness row and its shard-head advance commit in the *same transaction* as the
  source event — for the admission append that gates key attachment, for reseal
  rewrap events, and for D3b open events alike. A separate or asynchronous append
  would let a crash between them drop exactly the row an attacker wants missing.
  E2 therefore wires the witness call into the reseal orchestrator and the D3b
  open function as well as admission; covering only admission does not satisfy
  this. It is an E2 release prerequisite, not something E3's kill matrix
  discovers. **A future slice that adds a fourth source ledger inherits this
  obligation**, and a CI gate asserts that every source discriminator the witness
  record accepts has a wired-in transactional call site — the "outbox" naming
  trap that hid the reseal transaction shape will otherwise recur.
- **Comparison is operator-driven, and the docs say so.** No automated
  cross-consumer comparator is in scope. What this umbrella guarantees is that
  the material needed for comparison exists, is signed, and is independently
  verifiable offline; E2 ships a verifier tool so an operator can actually run
  it. Slices may claim "detectable by comparison," never "detected."
- **Comparison depends on someone actually retaining the stream.** With no
  consumer registry, aimee cannot know whether anyone kept anything. The
  "detectable by comparison" property is therefore a property of the deployment's
  surrounding collector ecosystem, not of aimee alone, and operator documentation
  must say so rather than implying the guarantee travels with the software.

- **Evidence is durably committed before key use.** This is the existing P7
  admission rule and the actual fail-closed property. No slice may weaken it, and
  no slice may add a second, weaker gate that appears to replace it.
- **Chain verification is default-on and non-disableable on a key-holding kb.**
  It fails closed at boot if verification is off. This is a build-time property,
  not an operator knob.
- **No secret, key, or fingerprint of a key is ever exported.** Records carry
  identity, timestamp, `provider:cred`, request id, and hashes. Every slice's
  validation includes canary scans of the bytes actually exported.
- **No runtime DDL.** Shard counter rows are created by DML upsert, consistent
  with the existing no-DDL invariant.
- **Shared state only.** The evidence log and shard counters are PostgreSQL
  state. Nothing load-bearing lives in instance-local memory or on an instance's
  disk, because autoscale teardown destroys it.
- **The hot path is bounded; checkpoint cost is off it and explicitly capped.** A
  shard advance writes two rows — the evidence record and the shard head — and
  touches no tree state. Checkpoint generation builds a depth-64 sparse Merkle
  tree by scanning the shard-head table once per cadence, bounded by a documented
  maximum shard count with a typed failure above it. The scan is stated, not
  hidden behind "incrementally maintained": what matters is that it is off the
  admission path and capped, not that no scan exists. Concrete shape, depth,
  schema, and proof bounds are in E1 §2 and §6. Every emission call carries a
  monotonic deadline.
- **Signature verification requires an out-of-band trust anchor.** Consumers must
  obtain checkpoint verification keys through a channel independent of the
  emitting host. A signature checked against a key taken from the same surface as
  the checkpoint proves nothing against a host attacker, who can substitute both.
  Signatures are integrity-of-transport and rotation hygiene; they are not the
  defense against host compromise. Comparison is.

## Threat model and honest residual

To forge a consistent history, an attacker must rewrite aimee-kb's chain *and*
every copy already exported to every downstream service, in a mutually consistent
way, without access to those hosts. Exported copies cannot be reached backwards
in time.

The forthcoming event-bus work raises that bar substantially rather than
marginally. Activity is recorded and replayable across services, so a forgery
must additionally stay consistent with that stream. The attacker would need
simultaneous control of the user's machine, aimee-server, and aimee-kb, including
the bus itself, holding all of it mutually consistent — and consistent with what
downstream collectors already hold. Short of that, the record remains replayable
up to the moment of full takeover. This assurance argument does not depend on any
single service being trustworthy, which is the point.

The residual is bounded and stated plainly: an attacker with full simultaneous
control of all three systems and the bus controls what is recorded and exported
from that moment forward, so activity after that point has no independent copy.
Everything before it remains detectable and replayable, because those copies are
already beyond the attacker's reach. No slice may claim the residual is zero.

## Validation gates (umbrella level)

- **Unit/default build:** record, checkpoint, and inclusion-proof vectors are
  exact and stable; rendering is deterministic; tampered leaves, reordered leaf
  sets, and forged proofs are rejected with distinct typed reasons.
- **ASAN/UBSAN:** record encode/decode fuzz and every rendering success/failure
  path; no leak, no use-after-cleanse, no callback after a fail-closed
  transition.
- **Real PG17 on CT103:** in-place schema upgrade plus the full P1 RLS gate;
  concurrent shard appends produce gap-free sequences with correct linkage;
  the witness row and shard head commit atomically with the source event or not
  at all, proven separately for all three source ledgers; forced disconnect at
  each transaction boundary never advances a shard head without its evidence row;
  a shard advance writes exactly two rows; checkpoint construction at the
  documented shard ceiling completes within its deadline; append-only rules
  reject update/delete/truncate on every new table.
- **CT260 real daemon:** the E3 kill matrix against a real aimee-kb process, real
  PG17, swtpm, and a log/OTLP consumer **configured to durably retain the stream**
  — proving that a rewritten local chain is caught by comparison against
  previously emitted copies. The retention configuration is part of the gate: run
  with a consumer that drops or samples and the comparison result proves nothing,
  because the copies it would compare against were never kept. A gate that can be
  passed by a dropping consumer is not a gate.
- **Canary scan:** database, files, logs, crash artifacts, and the bytes actually
  emitted.
- **Offline verifier:** the E2 verifier tool validates a checkpoint, an inclusion
  proof, and a record range using only emitted bytes and an out-of-band trust
  anchor, with no access to aimee's database, and correctly reports a planted
  divergence.

## Deferred beyond this umbrella

- Automated continuous cross-consumer comparison; it stays an operator-driven
  incident procedure.
- Event-bus record/replay integration, which supplies the reconstruction half of
  the story and is owned by its own proposal.
- Operator-facing witness console surfaces beyond the existing P5-D status
  plumbing.
- KMS/PKCS#11 fleet root activation, which remains owned by the reseal umbrella.
