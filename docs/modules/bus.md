# Event-bus module

**Owner:** runtime core
**Paths:** `src/modules/bus/`, `server-go/bus/`

## Owns

- wire frame and version negotiation;
- control, private queue-pair, and arena layouts;
- SPSC rings and bounded credits;
- client admission and reap;
- publish, subscribe, request/reply, cancellation, and typed absence;
- host sequence, observer routing, and full-stream tap;
- capture format and observational replay;
- C/Go vectors and conformance.

## Does not own

- network transport between services;
- module business schemas beyond stable kind registration;
- credential or user authentication policy;
- WORM storage or external witnessing;
- workflow scheduling;
- deterministic module execution replay;
- hostile-code sandboxing.

## Contracts

Per-source FIFO is guaranteed. The host stamps global accepted order before routing. `publish` success
means accepted into the producer ring, not consumed or durable. Backpressure is bounded and declared
per kind. Arena references use generation and holder checks and are released on consumer completion
or reap.

The tap is the only core full-stream observer. Ordinary clients receive only authorized kinds.

## Required checks

- wire unit tests and corrupt-frame rejection;
- C/Go golden vectors and live conformance;
- ring/arena ASAN and TSAN lanes;
- admission and cross-client isolation;
- flow control, overflow, cancellation, and reap;
- capture classification and byte-exact arena materialization;
- audit durability, retention, replay, and shutdown race;
- dispatch performance gate.

See [Event bus](../EVENT_BUS.md) for the working guide.
