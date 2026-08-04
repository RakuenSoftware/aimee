# Event-bus third-language conformance residual

- **State:** PENDING — residual scope only.
- **Archived parent:** [`event-bus-wire-spec.md`](../done/event-bus-wire-spec.md).

## Delivered foundation

The v0 bus has one C host, independent C and pure-Go clients, frozen language-neutral vectors,
cross-process interop, flow-control and recovery coverage, capture, and shipping module-runtime
consumers.

## Remaining work

Write a client in a language other than C or Go using only the archived normative specification and
committed wire vectors. It must not link, import, generate from, or shell out to either reference
implementation.

## Acceptance

The client independently encodes and decodes every positive and negative vector, attaches to the C
host, exchanges notification and request/reply traffic, handles `capability_absent`, cancellation,
backpressure, and reaped-client recovery, and runs from the normal conformance entrypoint.

