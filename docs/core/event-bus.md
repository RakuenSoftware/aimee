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

External C module repositories consume the host-free
`aimee-core-event-bus-client` target. Go modules use the pure-Go client and
process runtime under `server-go/bus`; it mirrors authenticated attach, AMOD
fragmentation, deadlines, cancellation, bounded concurrent handlers,
heartbeats, and host-epoch shutdown without cgo. The shared host runtime in `bus_runtime.c`
owns the listener, peer admission, heartbeat, and reap lifecycle. Server and KB
configure separate endpoints at `<config>/<daemon>-module-bus.sock` and separate
policies at `<config>/modules.d/<daemon>`; the environment can override those
with `AIMEE_MODULE_BUS_SOCKET` and `AIMEE_MODULE_POLICY_DIR`.

`src/modules/process-contracts.json` is also the implementation-language switch
for supervised processes. Ten migrated batches—`memory`, `learning`,
`routing`, `delegates`, `tools`, `workspace`, `git`, `skills`, and
`response-composition`, followed by `governance`, `workflows`, `roundtable`,
`kb-synthesis`, `runtime-web`, `control-web`, and `benchmarks`—put every process
identity on the Go multicall
executable in `server-go/cmd/aimee-module`; its basename selects an isolated
identity and one module package under `server-go/modules`. The runtime bundle emits no C process
source for those entries. Their C `module_adapter.c` files serve only as
wire-parity fixtures while the deeper module-owned C surfaces are migrated in
later batches. The workflows process owns only the deterministic advance
admission classification; it is not a second workflow lifecycle runtime.
The KB synthesis process similarly owns only the pure grounding decision, not
the curator worker, provider, or persistence lifecycle.
The runtime-web process owns only RPC fault classification. The physical Go web
provider imports the same policy package; it remains the owner of HTTPS,
browser authentication, sessions, routing, proxy transport, and assets.
The control-web process owns only proxy-route authorization. Its physical Go
provider imports the same policy package; HTTPS, OIDC/session handling, CSRF,
credential selection, proxy transport, and assets remain provider-owned. The
KB's C console-admin allowlist remains an independent defence-in-depth check.

Every strict `*.grant` policy binds one principal class/reference to an exact
absolute executable (resolved and compared with Linux `SO_PEERCRED` and
`/proc/<pid>/exe`), an expected UID, and explicit publish, subscribe, request,
and serve kind lists. The host installs all grants before returning mappings,
overwrites client-claimed source/principal fields, and returns a typed
capability-denied event when a module attempts undeclared fresh output.

The attach socket is mode `0600` and used only for the descriptor handshake.
All event traffic remains in the daemon's local shared mappings; there is no
server-to-KB bus and no cross-machine shared memory.

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
