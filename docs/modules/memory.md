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
  An image update is refused while the application is running; a changed account list, shared
  scope or embedder found while it runs keeps the current installation until the next start.

A Server serves personal memory for `aimee`, every browser login and any account in
`AIMEE_MEMORY_ACCOUNTS`, at most 32. A KB serves `global` and the scopes in
`AIMEE_MEMORY_SHARED_SCOPES` (by default every project and workspace), storing shared facts as
written. The supervisor passes `AIMEE_MODULE_PLACEMENT` to module
processes: `server` serves the instance-local user scope, `kb` serves the shared scope. Memory is
selected at deployment, not switched at run time:

- `runtime_toggle.supported`: `false`; the installation is replaced only by provisioning while the
  application is stopped.

A Server enrolled into a separately deployed KB reaches it over HTTPS, and that link never carries
memory. Such a Server serves personal memory only, as in 1.0: a shared-store request (`store=kb`)
answers the typed, retryable `unavailable` result ("memory bus service unavailable or
unsupported", dependency `kb`), and with `kb_mode` `remote` and automatic recall on, a native run
refuses its memory context rather than continue without the shared lane. Set `kb_mode` to `none`
for private-only context on that Server. Shared memory is written and read on the KB itself.

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
At each start the resource composes its module grant policy from the installation's grants and the
grants an operator keeps in `$AIMEE_HOME/modules.d/<role>`, such as an installed plugin's; the
installation's grants win, and an operator grant for a principal the installation owns, or for the
public memory adapter it replaces, is left out and named in the startup log.
Host-side checks do not move with the implementation: source revalidation, the byte budget
(`request_budget_exceeded`), the send guard and provider receipts stay in `src/wire_fence.c` and
`src/server/ingress_preinject.c`. The bundle is verified against its recorded hashes before it is
linked (`scripts/check_memory_bundle.py`).

## Supported journeys

Operators start the composition through `scripts/compose-local.sh`, wait for readiness and use the
ordinary memory commands.

## Tests and failure behavior

`scripts/tests/test_memory_provision.py` covers the provisioning decisions,
`scripts/tests/test_check_memory_bundle.py` the bundle verification and
`scripts/tests/test_compose_vault_init.py` the startup order. `make -C src lint` fails when a
public file the bundle was compiled against has changed, naming the file, so a stale bundle is
never linked.

The Docker deployment tests (`tests/e2e/deployment-matrix.py`, `egress-vault-e2e.py`,
`kb-upgrade-e2e.py`) run the shipped image with its provisioned memory, as an operator would.

Two bare-metal rigs test host-side gates, not memory, and need some memory module behind them:
the live write-tier rig (`AIMEE_LIVE_REFERENCE_MEMORY_OWNER`) and the execution-grade journey.
They use the frozen 1.0 Go owner as a test double. [Its public source pin](../../tests/fixtures/legacy-memory-owner.json)
is separate from the shipped implementation; with Go 1.26.10 available, run
`python3 scripts/build_reference_memory_fixture.py --output /absolute/test-cache/aimee-module-memory`
from the repository root (create the output directory first) and check an existing artifact with
`--verify`. The reference is never installed or shipped. The Docker tests still accept
`--reference-memory-owner` to qualify against it explicitly; that run uses the `public-reference`
profile of `tests/e2e/memory-placement-e2e.py`, and a failed request never switches profiles.

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
`vendor/aims-memory/bundle.json`; a change to one of them needs a rebuilt bundle. Bundles exist
for `linux-amd64` and `linux-arm64`, and the image build takes the one for its target platform;
an image for any other platform carries no memory and cannot provision it. In the `kb` placement the scopes in `AIMEE_MEMORY_SHARED_SCOPES` are served (by default every
project and workspace);
a shared store that names no scope is global, as in 1.0.x. The frozen reference in `tests/fixtures/legacy-memory-owner.json` records the last public Go
owner for compatibility tests only; it is not a default or a runtime fallback.

## Extension and removal

A new bundle release replaces the archive under `vendor/aims-memory` and is recorded with
`scripts/check_memory_bundle.py`; deployments pick it up at their next start after the image
update. The unavailable adapter stays in the public tree so that an instance without a provisioned
installation fails closed and visibly, as `missing_module: memory`, and never falls back to a
built-in datastore.
