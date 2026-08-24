# Proposal: a complete record of what crossed the bus

- **State:** DONE - implemented and validated 2026-08-24.
- **Author:** JBailes.
- **Charter roles:** Review, Persist, Gate-Promote.
- **Owns:** the durability contract for bus traffic and the distinction between diagnostic capture
  and the durable audit record.
- **Depends on:** the C event-bus core and observability bridge, the WORM audit ledger,
  `src/modules/process-contracts.json`, and `bench/bus_baseline.json`.

## Problem

The bus is where supervised module requests, replies, and notifications become ordered and observable.
That makes it the natural place to answer what crossed a daemon boundary. The repository previously had
two different records:

| Record | Carries | Durable |
| --- | --- | --- |
| WORM audit ledger | governed actions and guardrail events | yes |
| Capture stream | every routed event in sequence order | no |

That split is intentional. Capture is useful for diagnostics and replay, while the WORM ledger is the
security record. The failure was asking capture to carry evidence that no other record retained.

Capture could be absent from startup, abandoned after an I/O or allocation failure, or pruned after 16
newer sessions. Those transitions produced only warnings. Worse, overflow and producer-reap controls
that proved the bus had discarded an event went only to the same losable tap. A reviewer could not
distinguish an idle period from an absent record.

Module calls also had no general durable seam. Consequential decisions such as typed-fact writes, PII
recall, learning sink selection, skill matching, workflow admission, governance tool policy, and final
execution policy depended on a caller-specific audit emission, if one existed.

Finally, not every component crosses the bus. Core components call each other in process, so a claim of
whole-daemon completeness would exceed what the mechanism can observe.

## Decision

Deliver four ordered slices.

| Slice | Decision |
| --- | --- |
| C0 | Make capture absence and pruning first-class durable facts and expose capture health. |
| C1 | Write overflow and producer-reap loss controls to the WORM ledger. |
| C2 | Require every supervised event kind to declare `ledger`, `capture`, or `sampled` durability. |
| C3 | Close every declared ledger gap, beginning with fail-closed decision seams. |

### C0: capture gaps are facts

The observability bridge writes `bus.capture.gap` with session, last successfully flushed sequence,
reason, and wall time for `no_home`, `open_failed`, `write_failed`, and `sink_broken`. Pruning writes
`bus.capture.pruned` for every removed session. Both server and KB health expose `capture_ok`, reason,
session, and last sequence.

The marker is written directly to the daemon's WORM sink. It does not travel through capture and does
not depend on the failed layer recovering. A bounded in-memory retry queue retains startup markers when
the KB's PostgreSQL ledger is not ready yet; retries are rate-limited and shutdown makes one final
attempt.

### C1: loss escapes the losable layer

The C bus host retains its ordered tap controls and also calls a rare-event loss sink for overflow and
producer reap. `bus.overflow` names the lost sequence and event kind. `bus.producer_reaped` names the
discarded sequence, event kind, and source slot. This adds no durable write to ordinary dispatch.

### C2: durability is declared and checked

Every stage in `src/modules/process-contracts.json` carries:

- `durability`: `ledger`, `capture`, or `sampled`;
- `durability_reason` explaining the choice;
- `emitter` for ledger kinds;
- a declared parts-per-million sampling rate when `sampled` is used.

`scripts/check_event_durability.py` resolves every public module event declaration, checks the complete
contract catalog, and requires the runtime ledger table to match the declared ledger set exactly. It
fails on an undeclared kind, a ledger kind without the module request/reply emitter, a stale runtime
entry, and zero resolved kinds. Its unit tests run in the same `make lint` target.

### C3: close the named decision seams

The sole C-to-module request/reply seam emits durable intent and reply records for every ledger kind.
Rows retain event kind, stage, trace identity, body sizes, result, and the SHA-256 digest of a successful
response. Raw request arguments and response bodies are excluded.

The final execution-policy seam is now a required Go module process on event `8449`. The C caller only
transports the request and applies the verdict. The Go handler owns operator policy, source-discovery
interception, and computer-use decisions. Missing capability, timeout, cancellation, invalid input,
invalid policy, and malformed responses deny. There is no local C policy fallback.

The declared ledger set covers the named memory, learning, skills, workflow, governance, and
execution-policy decisions, along with other stateful supervised stages. Pure transforms and explicitly
diagnostic stages remain `capture` with recorded reasons. The initial catalog contains 84 ledger kinds
and 17 capture kinds; no kind is currently sampled.

## Language and trust boundary

All module decision work added or moved by this proposal, including execution-policy, is Go. C remains
only where this change requires it: event routing, capture, loss detection, daemon integration, and
WORM sink bridges. A C compatibility caller may transport and enforce a Go verdict, but it may not
decide policy. The pre-existing DB1 and DB2 C process runtimes are not language migrations in this
proposal.

Seven components remain `execution: core`: `module-runtime`, `ir`, `translation`, `protocols`,
`gateway`, `vault`, and `audit`. Their in-process calls never cross the transport and are outside this
record's completeness claim. Explicit audit bridges owned by those components remain separate controls.

## Threat and failure model

| Failure | Control |
| --- | --- |
| No capture is read as no activity | Durable gap and prune markers plus health state. |
| Loss evidence disappears with capture | Durable overflow and reap rows. |
| A new kind ships without a durability decision | Contract lint fails the build. |
| The lint silently covers nothing | Zero resolved kinds is an error and has a regression test. |
| Cost creeps onto ordinary dispatch | Durable writes occur at declared module seams or rare failures; the bus benchmark remains gated. |
| A compromised host forges or suppresses records | Out of scope; an off-host witness or anchor remains required. |

## Non-goals

- Do not make best-effort capture durable.
- Do not move core components onto the bus as part of this change.
- Do not persist raw module request or response bodies.
- Do not add an unconditional WORM write to ordinary C bus dispatch.

## Acceptance

- Each of the four capture failure reasons produces a readable WORM row.
- Every pruned session produces a durable marker.
- Overflow and producer reap each produce a durable row with the discarded identity.
- `make lint` runs the durability checker and its non-vacuity tests and reports real counts.
- Every supervised event kind has an explicit durability class and reason.
- Execution-policy decisions are served by Go and fail closed through the C caller.
- Server and KB health make capture failure immediately queryable.
- The bus dispatch and audit enqueue performance ceilings remain green.
- A PostgreSQL two-service regression proves disabling capture makes a completeness assertion fail
  while the durable gap remains queryable.

## Outcome

Capture remains intentionally prunable. Its absence is no longer silent, loss evidence no longer
depends on it, and the durable coverage of every supervised event kind is a declared and mechanically
checked decision. The resulting claim is precise: the ledger can account for what crossed the bus, not
for calls that never used it.
