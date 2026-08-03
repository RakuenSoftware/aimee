# Core C event bus

**Owner:** runtime core
**Paths:** `src/core/event_bus/`, `server-go/bus/`

`libaimee-core-event-bus.a` is the single POSIX implementation consumed by
`aimee-server` and `aimee-kb`. Each daemon hosts an independent local bus for
the modules in its own container. The thin client does not link it, and the bus
never carries server-to-KB or thinclient-to-server traffic.

## Owns

- wire frame and version negotiation;
- control, private queue-pair, and arena layouts;
- SPSC rings and bounded credits;
- client admission and reap;
- publish, subscribe, request/reply, cancellation, and typed absence;
- host sequence, observer routing, and full-stream tap;
- capture format and observational replay;
- local `SOCK_SEQPACKET` module attachment and descriptor grant;
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

Public headers live under `src/core/event_bus/include/aimee/core/event_bus`.
`bus_attach.h` is shared attach wire state, so an external module client does
not include the host implementation. `bus_endpoint.h` creates the local attach
socket; after `bus_client_attach_as` completes, the socket is no longer the data
path and the module uses only its mappings. Admission and event-kind grants are
daemon policy injected into the core host.

External module repositories consume the host-free
`aimee-core-event-bus-client` target. The server or KB module launcher owns the
listener lifecycle and authenticates the process before translating its opaque
principal into attach policy; the core does not expose an unauthenticated
default socket. The host applies the attach hook and all kind grants atomically
before it passes any shared-memory descriptors.

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
