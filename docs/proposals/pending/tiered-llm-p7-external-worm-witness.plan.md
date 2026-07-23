# P7 external WORM witness and full kill matrix

- **State:** staged umbrella plan; the P2b production release gate stays closed
  until every slice merges.
- **Depends on:** P7-reseal D1, D2a, D2b, D3a, D3b (merged), P3a WORM ledger, and
  the existing kb audit chain.
- **Enables:** the last open P7 line item — `external WORM delivery + full kill
  matrix` — and therefore P2b production org egress.

## Why this is the remaining P7 scope

Everything P7 has landed so far makes evidence *correct inside PostgreSQL*. The
kb audit chain (`kb_audit_event`, `src/db2/kb_audit_worm.c`) is hash-chained and
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
2. **E2 — emission, continuous verification, and release-gate flip.** Checkpoint
   cadence ("every N entries or T seconds, whichever first"), metrics emission of
   signed checkpoints, log/OTLP emission of per-entry evidence, continuous chain
   verification with typed integrity alerts, boot fail-closed for any key-holding
   kb whose chain verification is off, and only then a real
   `kb_egress_release_allowed()`.
3. **E3 — full kill matrix.** The exhaustive CT103/CT260 restart and signal-level
   kill matrix across every P7 durable boundary — the reseal boundaries D3b
   enumerated plus the new evidence-append, shard-advance, checkpoint, and
   emission boundaries — with raw-key canary scans over database, files, logs,
   and crash artifacts.

Each slice gets its own reviewed plan, adversarial branch review, target
validation, and merge. E1's plan is
`tiered-llm-p7-witness-e1-record-checkpoint-evidence-log.plan.md`.

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

The security property is simple and does not require any downstream cooperation:

- A break in aimee's own chain — a bad link, a regressing sequence, a checkpoint
  that fails signature verification — proves tampering, locally, immediately.
- A privileged actor who instead rewrites *consistently*, or rolls the database
  back to an earlier valid state, produces a locally clean chain that
  **contradicts copies already exported to other machines**. Those copies are on
  hosts the attacker does not control and cannot reach backwards in time to
  amend. Comparison exposes the rewrite.

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

This reuses what P9a already built rather than inventing a transport. Signed
checkpoints ride the metrics surface — `GET /v1/metrics`, behind org-admin auth
or the constant-time scrape token (`src/kb/http/kb_http_telemetry.c`,
`src/headers/kb_http_telemetry.h`) — because they are small, bounded, and
constant-shape, which is what a Prometheus sample carries honestly. Per-entry
evidence rides the log/OTLP path, because Prometheus exposition is a sampled
snapshot: entries appearing between scrapes can be missed, and record bytes as
labels are unbounded cardinality.

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
  itself; a locally clean chain that contradicts an exported copy proves it just
  as well. No slice may describe a durable local append or the primary hash chain
  as sufficient on its own.
- **This umbrella delivers detection and bounding; the event bus delivers
  reconstruction.** No slice may claim the witness export alone rebuilds the
  original event history. Claims about determining what was done must cite the
  event-bus record/replay mechanism, not the chain.
- **Export everything, gate nothing on it.** All evidence is exported; export lag
  raises a typed integrity alert and never blocks admission. There is no delivery
  receipt, no watermark, no consumer registry, and no unwitnessed budget.
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
- **Bounded work.** Checkpoint generation is bounded by construction — an
  incrementally maintained Merkle root, never a full scan of every shard head —
  and every export call carries a monotonic deadline.

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
  forced disconnect at each transaction boundary never advances a shard head
  without its evidence row; append-only rules reject update/delete/truncate on
  every new table.
- **CT260 real daemon:** the E3 kill matrix against a real aimee-kb process, real
  PG17, swtpm, and real metrics/log consumers, proving that a rewritten local
  chain is caught by comparison against previously exported copies.
- **Canary scan:** database, files, logs, crash artifacts, and the bytes actually
  exported to every consumer.

## Deferred beyond this umbrella

- Automated continuous cross-consumer comparison; it stays an operator-driven
  incident procedure.
- Event-bus record/replay integration, which supplies the reconstruction half of
  the story and is owned by its own proposal.
- Operator-facing witness console surfaces beyond the existing P5-D status
  plumbing.
- KMS/PKCS#11 fleet root activation, which remains owned by the reseal umbrella.
