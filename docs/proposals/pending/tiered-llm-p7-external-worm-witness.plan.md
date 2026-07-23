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

1. **E1 — witness record, export form, shared outbox, and atomic budget.**
   Canonical witness record encoding and per-entry evidence digest, the
   signed-head checkpoint format, the deterministic exported rendering and
   per-consumer confirmed watermark, the durable shared outbox table,
   per-`(tenant, provider)` shard counter rows created by DML upsert (no runtime
   DDL), and the reservation of unwitnessed slots *inside* the admission
   transaction with release against the minimum confirmed watermark. No admission
   caller, no drain worker, no emission, no production invocation.
2. **E2 — drain worker, fail-closed admission gate, and release-gate flip.**
   Any-instance batched drain under CAS/lease ownership, the periodic global
   checkpoint that signs and links sub-chain heads, continuous chain
   verification, boot fail-closed for any key-holding kb whose witness is
   unconfigured or whose chain verification is off, and only then a real
   `kb_egress_release_allowed()`.
3. **E3 — full kill matrix.** The exhaustive CT103/CT260 restart and signal-level
   kill matrix across every P7 durable boundary — the reseal boundaries D3b
   enumerated plus the new witness reservation, drain, acknowledgement, and
   checkpoint boundaries — with raw-key canary scans over database, files, logs,
   and crash artifacts.

Each slice gets its own reviewed plan, adversarial branch review, target
validation, and merge. E1's plan is
`tiered-llm-p7-witness-e1-record-outbox-budget.plan.md`.

E1 and E2 were originally split so that the encoding/acknowledgement rules could
be proven before any budget depended on them. They are folded because the budget
is meaningless without the outbox it counts and the outbox is dead weight without
the budget that bounds it; the separation moved review cost without moving risk.
The compensating control is that E1 still ships **no caller** — nothing drains,
nothing gates, and the release gate stays closed until E2.

## Witness architecture: stored on aimee-kb, exported to many consumers

**Decision:** the witness store is held on aimee-kb. aimee-kb exports the evidence
outward as metrics and logs over the existing telemetry surfaces, and the
independent verification points are the **multiple downstream services that
receive those exports**. There is no third-party append-only sink and no new
external service dependency.

Tamper independence therefore comes from **fan-out and divergence detection**, not
from a single privileged remote store. Evidence leaves aimee-kb continuously, to
more than one consumer, in a form each consumer can verify on its own. An attacker
who rewrites the in-database chain must also make every downstream copy agree —
across services with different retention, different operators, and different
storage — and any single disagreeing copy is the tamper signal.

This reuses what P9a already built rather than inventing a transport: kb serves
Prometheus text at `GET /v1/metrics` behind either org-admin auth or the
constant-time scrape token (`src/kb/http/kb_http_telemetry.c`,
`src/headers/kb_http_telemetry.h`), and P9's forwarder/OTLP work extends the same
seam. Witness export rides those surfaces.

Three consequences E1 must resolve, because they are where this architecture is
genuinely harder than a cooperative sink:

- **Exported evidence must be self-verifying.** A consumer that only sees counters
  can detect nothing. What is exported must include the signed checkpoint and the
  per-shard head hashes, so any consumer — with no access to aimee-kb's database —
  can verify linkage and signature offline, and so two consumers can be compared
  byte-for-byte.
- **Pull-based export gives no acknowledgement.** A Prometheus scrape does not tell
  kb that evidence was durably retained. The unwitnessed budget cannot be released
  by "we rendered it into a metrics page." E1 must define what counts as a
  confirmed export watermark per consumer, and the budget releases only against
  that — never against a render, a scrape hit, or an elapsed timer.
- **Detection is local; reconstruction is the external job.** Continuous
  cross-consumer comparison is not required and is not proposed. Chain
  verification on aimee-kb detects the break; the exported copies exist so that,
  once a break is detected, an operator can reconstruct the true history from
  sources the attacker did not control. E1 must therefore make the exported form
  sufficient for that reconstruction — enough linkage and signed anchors to
  re-derive what the chain should have contained — not merely enough for a
  dashboard.

The residual is stated plainly: an attacker who owns the kb host at the moment of
export controls what is exported from then on. Fan-out bounds retroactive rewriting
of already-exported evidence — it does not protect evidence not yet exported. The
unwitnessed budget is exactly what bounds that window.

## Non-negotiable invariants

- **No single source of authority: local detection, external adjudication.** The
  tamper-evident chain on aimee-kb is what *proves tampering occurred* — a broken
  link, a regressing sequence, or a checkpoint that fails signature verification
  is detectable on aimee alone, without consulting anyone. What aimee cannot do
  by itself is establish what the history *should have been*, because an attacker
  who rewrites the chain can rewrite it consistently. That is what the exported
  copies are for: once a break is detected, the external sources are consulted to
  reconstruct the true history and bound the damage.
- **Detection must not depend on export; reconstruction must not depend on
  aimee.** Chain verification runs continuously on aimee and raises a typed
  integrity alert with no external round-trip. The exported evidence must be
  independently verifiable — signed checkpoints and head hashes — so a consumer
  can validate it without access to aimee's database.
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
- **Export accounting is idempotent and cannot manufacture success.** Re-exporting
  an entry, or replaying a retention confirmation, must not turn an uncommitted
  transition into a reported success, must not release a slot twice, and must not
  advance a watermark past evidence a consumer has not retained.
- **No secret, key, or fingerprint of a key ever reaches the witness.** The
  witness carries identity, timestamp, `provider:cred`, request id, and hashes.
  Every slice's validation includes canary scans of what actually left the host.
- **No runtime DDL.** Shard counter rows and budget rows are created by DML
  upsert, consistent with the existing no-DDL invariant.
- **Shared state only.** Outbox, budget, counters, and drain ownership are
  PostgreSQL state. Nothing load-bearing may live in instance-local memory or on
  an instance's disk, because autoscale teardown destroys it.
- **Bounded work.** Every drain batch, export call, and checkpoint has a monotonic
  deadline; deadline exhaustion leaves the reservation intact and the entry
  unexported rather than counted as retained.

## Honest residual, restated

A database compromise that erases an admission row and its not-yet-drained outbox
entry before the drain runs leaves that one use unwitnessed. The fail-closed
backlog bound caps how many uses can sit undrained; the highest-assurance posture
drains synchronously. The window is bounded, not zero, and no slice may claim
otherwise in code comments, docs, or operator-facing status.

## Validation gates (umbrella level)

- **Unit/default build:** unconfigured export fails closed; record encoding, digest,
  and checkpoint vectors are exact and stable; every idempotent transition
  replays identically; typed errors are distinguishable.
- **ASAN/UBSAN:** record encode/decode and every drain success/failure path; no
  leak, no use-after-cleanse, no callback after a fail-closed transition.
- **Real PG17 on CT103:** in-place schema upgrade plus the full P1 RLS gate;
  concurrent admissions racing the budget ceiling; shard starvation; drain
  ownership collision; forced disconnect at each transaction boundary;
  append-only rules reject update/delete/truncate on every new table.
- **CT260 real daemon:** the E3 kill matrix against a real aimee-kb process, real
  PG17, swtpm, and a real kb-hosted witness store.
- **Canary scan:** database, files, logs, crash artifacts, and the bytes actually
  exported to every consumer.

## Deferred beyond this umbrella

- Automated continuous cross-consumer comparison; reconstruction stays an
  operator-driven incident procedure.
- Operator-facing witness console surfaces beyond the existing P5-D status
  plumbing.
- KMS/PKCS#11 fleet root activation, which remains owned by the reseal umbrella.
