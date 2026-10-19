# Event-bus third-language conformance residual

- **State:** REJECTED. Archived 2026-08-14.
- **Archived parent:** [`event-bus-wire-spec.md`](../done/event-bus-wire-spec.md).

## Decision

This residual is rejected under the current implementation policy: pending proposal work must be
implemented in Go or moved to `rejected/`. Its sole purpose is to prove the language-neutral wire
contract with a client written in a language other than C or Go. A Go implementation would merely
duplicate the existing pure-Go reference client and would not satisfy the proposal's acceptance
criterion.

This is a policy incompatibility, not evidence that the event-bus wire contract is unsound. The C
host, independent C and pure-Go clients, frozen language-neutral vectors, cross-process interop,
flow-control and recovery coverage, capture, and shipping module-runtime consumers remain in place.

## Outcome (2026-10-19)

The owner directed a Rust reference client, and `rust/aimee-bus` delivers the deliverable below in
substance: a client in a third language that attaches to the single C host and interoperates, plus a
module-process runtime the C module-runtime caller drives. It links, imports, generates from and shells
out to neither reference implementation, and `scripts/check_bus_single_host.sh` enforces that.

One clause is not met as written. The frame codec was written from the specification and is held to
every committed vector, but region, ring and attach layouts are not in the specification's prose, and
the vectors cover frames only. Those layouts were taken from the C public headers that freeze them
with static asserts. See
[D11](../../dev/EVENT_BUS_DECISIONS.md#d11-the-rust-reference-client-is-a-third-independent-client).
A language-neutral layout vector table would close the gap.

This file stays in `rejected/` because the pending-audit manifests record it there as the archived
residual of the wire spec.

## Rejected deliverable

Write a client in a language other than C or Go using only the archived normative specification and
committed wire vectors, without linking, importing, generating from, or shelling out to either
reference implementation. The client would independently exercise every positive and negative
vector plus host interoperation, cancellation, backpressure, and reaped-client recovery.
