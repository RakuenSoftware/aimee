# memory module

## Purpose and non-goals

Memory stores and retrieves what an instance has learned: personal records on a Server, shared
records on a KB. From 1.1.0 it is served by a separate implementation that ships inside the
application image as a vendored binary bundle (`vendor/aims-memory`) and is provisioned for each
deployment at first boot. The implementation's source is not part of this repository.

This repository owns the generic SDK, the host paths that call memory, the bundle's packaging and
verification, and the provisioning command. It does not own the datastore, retrieval policy,
ontology or maintenance, and the retired Go memory owner is not restored. The public tree still
builds an unavailable adapter (`server-go/modules/memory/adapter/unavailable.go`), which is what
runs when no installation has been provisioned. See [Memory boundary](../MEMORY.md).

## Public contracts

The public `server-go/memory` SDK (`client.go`, `store.go`, `wire.go`, `scope.go`, `version.go`)
defines the generic client, store interface, wire types, scope selection and record versions an
implementation speaks. The implementation declares the public `memory.*` commands it serves; the
host exposes exactly those through the fixed `/v1/memory/*` routes and
`/v1/commands/memory.<verb>`. A command that is not declared returns `404` from the command route.
Legacy stage IDs alone do not establish protocol compatibility.

## Dependencies and consumers

The descriptor `src/modules/memory/module.yaml` declares no dependencies. The SDK uses a borrowed,
already admitted bus caller and stays independent of server domain storage. Consumers are the C
resource hosts, which authenticate callers and forward memory commands, and the host paths of a
provider-bound request in `src/server/ingress_preinject.c` and `src/wire_fence.c`, which ask the
implementation for context, a source release and revalidation. The bundle's bridge objects are
linked into `aimee-server` and `aimee-kb` by `Dockerfile.server`.

## Providers and readiness

Each role's supervisor requires a `memory` module. A provisioned installation supplies it together
with its database process; readiness is reached when both attach to the bus and the
implementation declares its commands. Without a provisioned installation the unavailable adapter
runs: it prints `memory capability unavailable` and exits, the supervisor restarts it, the instance
reports `missing_module: memory` in `/v1/ready`, and memory commands answer `503`.

Vector retrieval needs the embedding provider. Provisioning pins the provider by its identity, read
from the embedder image by `scripts/embedder-identity.py`; a composition without a local embedder
service is provisioned without vector retrieval.

## Configuration and activation

The one-shot `aimee-memory-init` service in `compose.yaml` runs `aimee-memory-provision`
(`deploy/container/memory-provision.py`) before the application starts:

- first boot: it generates this deployment's memory identity, grants and owner key, prepares the
  installation under `/opt/aims-memory/<role>` in the `aimee-memory` volume, and creates the
  writable subtree `private-memory/<role>` in the home volume;
- later boots: it verifies the installation and changes nothing;
- after an image update, or when the requested accounts or embedding provider change: it prepares
  the installation again under the same identity and keeps the owner key and the stored memory.
  This is refused while the application is running.

A Server serves personal memory for `aimee`, every browser login and any account in
`AIMEE_MEMORY_ACCOUNTS`, at most 32. A KB serves `global` and each scope in
`AIMEE_MEMORY_SHARED_SCOPES`, storing shared facts as written. The supervisor passes `AIMEE_MODULE_PLACEMENT` to module
processes: `server` serves the instance-local user scope, `kb` serves the shared scope. Memory is
selected at deployment, not switched at run time:

- `runtime_toggle.supported`: `false`; the installation is replaced only by provisioning while the
  application is stopped.

## Surfaces

The image carries the memory executables and provisioning scripts under `/opt/aimee/memory` and
the provisioning command as `/usr/local/bin/aimee-memory-provision`. The public tree also builds
the unavailable adapter, installed as `/usr/local/libexec/aimee-modules/aimee-module-memory`, and
the generic probe `server-go/cmd/aimee-memory-bus-probe`. CLI, MCP and HTTP memory surfaces are
host surfaces that dispatch to whatever the implementation declares.

## Data and migrations

The implementation keeps its records in its own database under `private-memory/<role>/state` in
the home volume, next to its configuration and owner key; one backup of the home volume covers
them. The `aimee-memory` volume holds only prepared artifacts and can be regenerated. Canonical
source producers, access, erasure and replay records remain with DB1 under
`server-go/modules/aimee/families/`.

The 1.0.x memory tables (`user_memories` on a Server, `memories` on a KB) are retained by an
upgrade but are no longer read; `scripts/upgrade/migrate-memory.py` exports their active records
and imports them through the public store command. See
[Upgrading](../UPGRADING.md#upgrade-from-102).

## Security and privacy

The application mounts the prepared installation read-only and can write only its configuration,
state and runtime directories. Every module process runs under an exact executable grant generated
at provisioning, and the owner key is created on the deployment and never leaves its home volume.
Host-side checks do not move with the implementation: source revalidation, the byte budget
(`request_budget_exceeded`), the send guard and provider receipts stay in `src/wire_fence.c` and
`src/server/ingress_preinject.c`. The bundle is verified against its recorded hashes before it is
linked (`scripts/check_memory_bundle.py`).

## Supported journeys

Operators start the composition through `scripts/compose-local.sh`, wait for readiness and use the
ordinary memory commands. Deployment compatibility tests can explicitly select the actual
historical Go owner as an external reference fixture. [Its frozen public source pin](../../tests/fixtures/legacy-memory-owner.json)
is separate from the shipped implementation. With Go 1.26.10 available, run
`python3 scripts/build_reference_memory_fixture.py --output /absolute/test-cache/aimee-module-memory`
from the repository root; create the output directory first. The builder exports the pinned source
into a disposable directory outside the checkout and writes a fresh executable plus a `.json`
provenance sidecar recording its source commit and binary SHA-256. This reference is not installed
or shipped by the builder.

## Tests and failure behavior

`scripts/tests/test_memory_provision.py` covers the provisioning decisions,
`scripts/tests/test_check_memory_bundle.py` the bundle verification and
`scripts/tests/test_compose_vault_init.py` the startup order. `make -C src lint` fails when a
public file the bundle was compiled against has changed, naming the file, so a stale bundle is
never linked.

Deployment tests can select the reference owner through an exact executable grant at
`/opt/aimee-test-memory-reference/aimee-module-memory`; other multicall roles retain their shipped
executables. The live write-tier authorization rig selects the same reference explicitly with
`AIMEE_LIVE_REFERENCE_MEMORY_OWNER=/absolute/test-cache/aimee-module-memory`.
`python3 scripts/build_reference_memory_fixture.py --verify /absolute/test-cache/aimee-module-memory`
checks an existing artifact without building or installing it.

Shared deployment qualification has two explicit profiles in `tests/e2e/memory-placement-e2e.py`:
`owner-client` retains the original Server HTTP/CLI/MCP forwarding and cwd/scope assertions, and
`public-reference` is chosen when the frozen fixture is selected. A failed request never switches
profiles or skips an assertion.

## Operational diagnostics

`GET /v1/ready` names the failed boundary: `missing_module: memory` means no implementation is
attached. Provisioning prints `[memory-provision]` lines saying whether the installation was
created, verified or updated, and a refusal says what to do. The supervisor log shows
`memory capability unavailable` each time it restarts the unavailable adapter.
`server-go/cmd/aimee-memory-bus-probe` exercises the generic bus contract against an attached
implementation.

## Compatibility

The wire types in `server-go/memory/wire.go` and the command names the host routes are the
compatibility surface. The bundle is tied to the exact public files recorded in
`vendor/aims-memory/bundle.json`; a change to one of them needs a rebuilt bundle. A bundle exists
for `linux-amd64` only, so an image for another platform carries no memory and cannot provision
it. In the `kb` placement only the scopes declared in `AIMEE_MEMORY_SHARED_SCOPES` are served;
a shared store that names no scope is global, as in 1.0.x. The frozen reference in `tests/fixtures/legacy-memory-owner.json` records the last public Go
owner for compatibility tests only; it is not a default or a runtime fallback.

## Extension and removal

A new bundle release replaces the archive under `vendor/aims-memory` and is recorded with
`scripts/check_memory_bundle.py`; deployments pick it up at their next start after the image
update. The unavailable adapter stays in the public tree so that an instance without a provisioned
installation fails closed and visibly, as `missing_module: memory`, and never falls back to a
built-in datastore.
