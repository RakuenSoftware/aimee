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
- `module_protocol`: the 40-byte AMOD envelope.
- `module_runtime::run_module_process`: attach as a module principal and serve stages, with request
  reassembly, reply fragmentation, absolute deadlines, cancellation, at most 16 concurrent handlers,
  contained handler panics, and a clean exit on shutdown or host restart.

Linux only, as the bus is. Arena-placed payloads need a host lease and arrive empty; module
processes use inline fragments instead, as the C and Go runtimes do.

## Independence

The crate depends on `libc` only, has no build script, and links nothing from the C or Go
implementations. `scripts/check_bus_single_host.sh` enforces that and holds it to client-only.
The frame codec follows the specification and the vectors. Region, ring and attach layouts follow
the C public headers that freeze them; see D11 in `docs/dev/EVENT_BUS_DECISIONS.md`.

## Tests

```bash
make -C src rust-bus-tests
```

That runs rustfmt, clippy with warnings as errors, the unit tests and the vectors, then the
interop legs: this client against the C host (`bus-conformance-host`), and the
`aimee-bus-module-helper` process under the C module-runtime caller (`unit-test-module-runtime`).
CI runs the same target in the `rust-bus-tests` job, which the `unit-tests` aggregate requires.
The toolchain is pinned in `rust/rust-toolchain.toml`.
