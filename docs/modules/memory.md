# memory module

## Purpose and non-goals

The public memory module packages a generic SDK, a bus probe and an unavailable adapter
(`server-go/modules/memory/adapter/unavailable.go`). It owns no datastore, retrieval policy,
ontology, maintenance, or source-release implementation. The bundled adapter exits before bus
attachment and advertises no handler or stages, so selecting its executable is not serving
readiness.

Aimee-specific memory behavior belongs in an installed memory implementation; reusable datastore
behavior belongs in an independent database product. Neither the canonical DB1 source producers
nor the generic adapter silently implement the retired Go memory owner. No private implementation
or database credentials are installed by this repository. See [Memory boundary](../MEMORY.md) for
the boundary statement and qualification responsibilities.

## Public contracts

The public `server-go/memory` SDK (`client.go`, `store.go`, `wire.go`, `scope.go`, `version.go`)
defines the generic client, store interface, wire types, scope selection and record versions an
implementation speaks. An installed implementation declares the public `memory.*` commands it
actually serves, for example store, get, list, search, delete, `memory.receipt` and
`memory.verify_receipt`; the host exposes exactly those through the fixed `/v1/memory/*` routes and
`/v1/commands/memory.<verb>`. A command that is not declared returns `404` from the command route,
and the bundled adapter declares none. Legacy stage IDs alone do not establish protocol
compatibility; the implementation must honestly declare its supported wire capabilities.

## Dependencies and consumers

The descriptor `src/modules/memory/module.yaml` declares no dependencies. The SDK uses a borrowed,
already admitted bus caller and stays independent of server domain storage. Consumers are the C
resource hosts, which authenticate callers and forward memory commands, and the host paths of a
provider-bound request in `src/server/ingress_preinject.c` and `src/wire_fence.c`, which ask the
installed implementation for context, a source release and revalidation.

## Providers and readiness

Each role's supervisor requires a `memory` module. With the bundled adapter the process prints
`memory capability unavailable` and exits, the supervisor restarts it, and the instance reports
`missing_module: memory` in `/v1/ready` with memory commands answering `503`. Readiness is reached
only when the implementation selected in the role module manifest attaches to the bus and
declares its commands. Public CI does not certify an installed implementation; its process,
datastore, provider-send and recovery qualification accompanies its deployment.

## Configuration and activation

Select an independently installed implementation through the existing role module manifest
(`server.modules` or `kb.modules` under the image's module-grant directory) with exact executable
grants. The supervisor passes `AIMEE_MODULE_PLACEMENT` to module processes: `server` serves the
instance-local user scope, `kb` serves global, workspace and project scopes. Memory is selected at
deployment, not switched at run time:

- `runtime_toggle.supported`: `false`; replacing the implementation means changing the manifest and
  restarting the role.

## Surfaces

The public tree builds one process for this module, the adapter installed as
`/usr/local/libexec/aimee-modules/aimee-module-memory`, and the generic probe
`server-go/cmd/aimee-memory-bus-probe`. CLI, MCP and HTTP memory surfaces are host surfaces that
dispatch to whatever the installed implementation declares; none of them is implemented by this
module.

## Data and migrations

This module owns no tables and ships no migrations. Canonical source producers, access, erasure
and replay records remain with DB1 under `server-go/modules/aimee/families/`. The 1.0.x memory
tables (`user_memories` on a Server, `memories` on a KB) are retained by an upgrade but are no
longer read; `scripts/upgrade/migrate-memory.py` exports their active records and imports them
through the installed implementation's public store command. See
[Upgrading](../UPGRADING.md#upgrade-from-102).

## Security and privacy

The adapter opens no storage and attaches to no bus, so it cannot disclose or mutate memory. An
installed implementation runs under exact executable grants in the role's grant directory and
receives only the calls the host admits for an authenticated caller. Host-side checks do not move
with the implementation: source revalidation, the byte budget (`request_budget_exceeded`), the send
guard and provider receipts stay in `src/wire_fence.c` and `src/server/ingress_preinject.c`.

## Supported journeys

Operators install an implementation, select it in the role manifest, wait for readiness and then
use the ordinary memory commands. Deployment compatibility tests can explicitly select the actual
historical Go owner as an external reference fixture. [Its frozen public source pin](../../tests/fixtures/legacy-memory-owner.json)
is separate from the shipped adapter. With Go 1.26.10 available, run
`python3 scripts/build_reference_memory_fixture.py --output /absolute/test-cache/aimee-module-memory`
from the repository root; create the output directory first. The builder exports the pinned source
into a disposable directory outside the checkout and writes a fresh executable plus a `.json`
provenance sidecar recording its source commit and binary SHA-256. This reference is not installed
or shipped by the builder.

## Tests and failure behavior

Deployment tests select the reference through an exact executable grant at
`/opt/aimee-test-memory-reference/aimee-module-memory`; other multicall roles retain their shipped
executables. Its historical SQL behavior does not establish readiness or parity for the default
unavailable adapter. The live write-tier authorization rig selects the same reference explicitly
with `AIMEE_LIVE_REFERENCE_MEMORY_OWNER=/absolute/test-cache/aimee-module-memory`. The shared live
helper verifies the sidecar before arming it and preserves every generated grant field except the
executable path. Other authentication rigs retain their owner-free configuration.
`python3 scripts/build_reference_memory_fixture.py --verify /absolute/test-cache/aimee-module-memory`
checks an existing artifact without building or installing it.

Shared deployment qualification has two explicit profiles. The default `owner-client` profile in
`tests/e2e/memory-placement-e2e.py` retains the original Server HTTP/CLI/MCP forwarding and
cwd/scope assertions and requires a compatible installed C owner client. Selecting the frozen
fixture makes the public Docker matrix choose `public-reference`: positive scope/state/authority
checks use the real authenticated KB owner transport, while Server KB paths must return typed
unavailability. Its qualification artifact records `server_forwarding_qualified: false`. Direct KB
owner checks do not qualify private Server forwarding or cwd propagation; those require the
separate `owner-client` run. A failed request never switches profiles or skips an assertion.

## Operational diagnostics

`GET /v1/ready` names the failed boundary: `missing_module: memory` means no implementation is
attached. The supervisor log shows `memory capability unavailable` each time it restarts the
bundled adapter. `server-go/cmd/aimee-memory-bus-probe` exercises the generic bus contract against
an attached implementation. Beyond that, diagnostics are the installed implementation's own.

## Compatibility

The wire types in `server-go/memory/wire.go` and the command names the host routes are the
compatibility surface. The frozen reference in `tests/fixtures/legacy-memory-owner.json` records
the last public Go owner for compatibility tests only; it is not a default or a runtime fallback.
Similar stage numbers do not imply compatibility with the retired owner's ABI.

## Extension and removal

A new implementation is added by installing its executable and selecting it in the role manifest;
nothing in this module changes. The bundled adapter stays as the explicit default so that a
deployment without an implementation fails closed and visibly, as `missing_module: memory`, and
never falls back to a built-in datastore. Removing an implementation returns the instance to that
unready state.
