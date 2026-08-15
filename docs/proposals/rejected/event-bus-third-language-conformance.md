# Event-bus third-language conformance residual

- **State:** REJECTED — archived 2026-08-14.
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

## Rejected deliverable

Write a client in a language other than C or Go using only the archived normative specification and
committed wire vectors, without linking, importing, generating from, or shelling out to either
reference implementation. The client would independently exercise every positive and negative
vector plus host interoperation, cancellation, backpressure, and reaped-client recovery.
