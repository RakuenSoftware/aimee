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
the hardened-vault proposal names in §6: the off-host append-only witness is what
makes tamper detection independent of the database. D3b said so explicitly —
neither a successful local outbox append nor the primary hash chain may be
described as external WORM delivery, and off-host witnessing remained a later
release gate.

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

1. **E1 — witness record, sink contract, and drain core.** Canonical witness
   record encoding and per-entry evidence digest, the signed-head checkpoint
   format, the transport-agnostic sink interface with a fail-closed default, and
   the idempotent delivery/acknowledgement state machine. No schema, no admission
   caller, no production invocation.
2. **E2 — shared witness outbox and atomic unwitnessed budget.** The durable
   shared outbox table, per-`(tenant, provider)` shard counter rows created by DML
   upsert (no runtime DDL), the reservation of unwitnessed slots *inside* the
   admission transaction, release-on-drain, RLS/grants, and replay/crash
   semantics. Still not gating production.
3. **E3 — drain worker, fail-closed admission gate, and release-gate flip.**
   Any-instance batched drain under CAS/lease ownership, the periodic global
   checkpoint that signs and links sub-chain heads, continuous chain
   verification, boot fail-closed for any key-holding kb whose witness is
   unconfigured or whose chain verification is off, and only then a real
   `kb_egress_release_allowed()`.
4. **E4 — full kill matrix.** The exhaustive CT103/CT260 restart and signal-level
   kill matrix across every P7 durable boundary — the reseal boundaries D3b
   enumerated plus the new witness reservation, drain, acknowledgement, and
   checkpoint boundaries — with raw-key canary scans over database, files, logs,
   and crash artifacts.

Each slice gets its own reviewed plan, adversarial branch review, target
validation, and merge. E1's plan is
`tiered-llm-p7-witness-e1-record-and-sink.plan.md`.

## Non-negotiable invariants

- **The witness is authoritative for tamper detection; PostgreSQL is not.** No
  slice may describe a durable local append, the primary hash chain, or a signed
  head held only in the database as external delivery.
- **Witnessing is never silently skipped on a key-holding kb.** The only operator
  tunable is the backlog bound. Whether to witness is a build-time property, and
  a key-holding kb fails closed at boot with witnessing or chain verification
  disabled.
- **The unwitnessed ceiling is global and shared, reserved atomically inside the
  admission transaction** — `UPDATE … SET unwitnessed = unwitnessed + 1 WHERE
  unwitnessed < :ceiling RETURNING` — never check-then-act, and never
  per-instance, because instance count is unbounded. A per-shard allowance sits
  under the global cap so one saturated shard cannot starve another tenant and
  one tenant cannot consume the whole global budget.
- **Budget exhaustion refuses new key use.** It never degrades to unwitnessed
  egress, and it never degrades to read-only-and-continue.
- **Delivery is idempotent and cannot manufacture success.** Re-delivering an
  entry, or acknowledging one twice, must not turn an uncommitted transition into
  a reported success, must not release a slot twice, and must not advance a
  checkpoint past evidence the sink has not accepted.
- **No secret, key, or fingerprint of a key ever reaches the witness.** The
  witness carries identity, timestamp, `provider:cred`, request id, and hashes.
  Every slice's validation includes canary scans of what actually left the host.
- **No runtime DDL.** Shard counter rows and budget rows are created by DML
  upsert, consistent with the existing no-DDL invariant.
- **Shared state only.** Outbox, budget, counters, and drain ownership are
  PostgreSQL state. Nothing load-bearing may live in instance-local memory or on
  an instance's disk, because autoscale teardown destroys it.
- **Bounded work.** Every drain batch, sink call, and checkpoint has a monotonic
  deadline; deadline exhaustion leaves the reservation intact and the entry
  undrained rather than acknowledged.

## Honest residual, restated

A database compromise that erases an admission row and its not-yet-drained outbox
entry before the drain runs leaves that one use unwitnessed. The fail-closed
backlog bound caps how many uses can sit undrained; the highest-assurance posture
drains synchronously. The window is bounded, not zero, and no slice may claim
otherwise in code comments, docs, or operator-facing status.

## Validation gates (umbrella level)

- **Unit/default build:** the sink stub fails closed; record encoding, digest,
  and checkpoint vectors are exact and stable; every idempotent transition
  replays identically; typed errors are distinguishable.
- **ASAN/UBSAN:** record encode/decode and every drain success/failure path; no
  leak, no use-after-cleanse, no callback after a fail-closed transition.
- **Real PG17 on CT103:** in-place schema upgrade plus the full P1 RLS gate;
  concurrent admissions racing the budget ceiling; shard starvation; drain
  ownership collision; forced disconnect at each transaction boundary;
  append-only rules reject update/delete/truncate on every new table.
- **CT260 real daemon:** the E4 kill matrix against a real aimee-kb process, real
  PG17, swtpm, and a real off-host sink endpoint.
- **Canary scan:** database, files, logs, crash artifacts, and the bytes actually
  written to the sink.

## Deferred beyond this umbrella

- Multi-witness quorum and cross-provider witness federation.
- Operator-facing witness console surfaces beyond the existing P5-D status
  plumbing.
- KMS/PKCS#11 fleet root activation, which remains owned by the reseal umbrella.
