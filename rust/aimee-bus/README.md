# aimee-bus

The Rust reference client for the Aimee shared-memory event bus, and a module-process runtime on top
of it. It is the third independent client of the single in-source C host, beside the C client
(`src/core/event_bus/bus_client.c`) and the pure-Go client (`server-go/bus`).

## What it provides

- `wire`: the 64-byte frame codec, held to `src/tests/fixtures/bus/wire_vectors.tsv`.
- `Client`: authenticated attach over `SOCK_SEQPACKET`, the three `SCM_RIGHTS` regions, publish,
  request and reply (with `F_MORE` fragments), cancel, poll, heartbeat, host-epoch detection, and
  a keep-alive for idle requesters. It is `Send + Sync`; emitters and pollers are serialised on the
  single-producer and single-consumer rings.
- Arena reads (D12): an arena-placed payload is copied out of the span its lease names, through the
  host's read-only lease view, and the reference is returned at once with a `LEASE_RELEASE`
  notification. From a host without the view, arena payloads arrive empty, as before.
- `module_protocol`: the 40-byte AMOD envelope.
- `module_runtime::run_module_process`: attach as a module principal and serve stages, with request
  reassembly, reply fragmentation, absolute deadlines, cancellation, at most 16 concurrent handlers,
  contained handler panics, and a clean exit on shutdown or host restart.
- `module_caller::ModuleCaller`: call a stage. Request fragmentation, reply reassembly with stage,
  trace and kind checks, a per-call response limit (an over-limit response is drained and reported
  with its length), absolute deadlines, caller cancellation (both publish `F_CANCEL`), stale-reply
  draining, and the host's capability-absent and capability-denied refusals. Its outcomes are the C
  core caller's (`aimee_module_call_result_t`); calls on one caller are serialised.

Linux only, as the bus is. Producing arena payloads stays co-located with the host; requesters and
module runtimes fragment inline, as the C and Go ones do.

## Independence

The crate depends on `libc` only, has no build script, and links nothing from the C or Go
implementations. `scripts/check_bus_single_host.sh` enforces that and holds it to client-only.
The frame codec is held to `wire_vectors.tsv`; region, ring, lease-view, attach and AMOD layouts to
`layout_vectors.tsv` (`src/layout.rs`). Both tables are generated from the specification, not from
either implementation; see D11 and D12 in `docs/dev/EVENT_BUS_DECISIONS.md`.

## Tests

```bash
make -C src rust-bus-tests
```

That runs rustfmt, clippy with warnings as errors, the unit tests and both vector tables, then the
interop legs: this client against the C host (`bus-conformance-host`, including arena reads), the
`aimee-bus-module-helper` process under the C module-runtime caller (`unit-test-module-runtime`),
and `aimee-bus-module-caller-check`, the Rust caller as its own admitted process, against the C
runtime's module and against the Rust module process.
CI runs the same target in the `rust-bus-tests` job, which the `unit-tests` aggregate requires.
The toolchain is pinned in `rust/rust-toolchain.toml`.

## Backend module adapter

`module_adapter` provides the communication adapter for independently owned
backends. Implement its `Backend` trait in the backend's repository and install
`Adapter::new(backend, stage_limits)?.handler()` in the existing
`ModuleProcessConfig`. Its opaque byte interface adds no application protocol.

```rust
use aimee_bus::module_adapter::{Adapter, Backend, Response, StageLimits};
use aimee_bus::module_protocol::ModuleStatus;
use aimee_bus::module_runtime::Invocation;
use std::{io::Write, sync::Arc};

struct Echo;
impl Backend for Echo {
    fn invoke(&self, invocation: &Invocation, request: &[u8], response: &mut Response)
        -> Result<(), ModuleStatus>
    {
        if invocation.cancelled() { return Err(ModuleStatus::Cancelled); }
        response.write_all(request).map_err(|_| ModuleStatus::Internal)
    }
}
let handler = Adapter::new(Arc::new(Echo), [StageLimits {
    stage_id: 1, request_bytes: 4096, response_bytes: 4096,
}]).unwrap().handler();
// The backend executable installs `handler` in its own ModuleProcessConfig.
```

The adapter validates nonempty, unique stage IDs and budgets within the AMOD
maximum, rejects unconfigured stages and oversized assembled requests before
backend dispatch, and supplies a bounded response writer. An ignored overflow
or allocation error still invalidates the whole reply. A backend failure drops
all partial output and returns only its AMOD status; it does not supply a body
for runtime diagnostic logging. `Err(ModuleStatus::Ok)` is an internal error.

The invocation preserves caller identity, trace, deadline and cancellation.
Authentication, fragmented transport, scheduling, panic containment and runtime
shutdown remain in the existing bus/runtime; the adapter adds none of them.
Backend handlers may run concurrently and must observe cancellation around bounded
units of work. Stage limits apply **after** runtime request reassembly; they do
not replace the runtime's 16 MiB assembly bound or constrain a backend's own
allocations. Cancellation cannot undo an operation already performed.

The adapter never retries. Backends own authorization, stable mutation IDs,
request meaning, storage and execution. A transport correlation ID is not an
application idempotency key. Process binaries, supervision, installation and
backend lifecycle also remain with their existing owners.

`tests/module_adapter.rs` drives the unchanged runtime with fragmented binary
payloads, caller metadata, limits, unknown stages, backend failures, panic
containment, deadline expiry and active cancellation. The existing C-host process
interop helper also uses this adapter, so `make -C src rust-bus-tests` exercises
it across real process boundaries in both C-caller and Rust-caller legs.
