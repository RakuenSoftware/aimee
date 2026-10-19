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
