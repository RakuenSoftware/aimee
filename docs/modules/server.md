# server module

## Purpose and non-goals

The Go `server` module composes the single-user Server role from standardized
modules. It owns role identity and process composition; the native protocol and
resource hosts still supply their existing C adapters. Composition does not
reimplement memory policy, PostgreSQL storage, or Vault in the role handler.

## Public contracts

`src/modules/server/include/aimee/server/module_api.h` declares identity event
`12801`, stage `1`, for principal `34`. The request is exactly
`{"operation":"identity"}`; the response contains version, role, and UUID.
Unknown fields, extra JSON values, unsupported stages, and mutation operations
return an invalid-request status. `server-go/modules/server/role.go` implements
this contract and delegates process lifetime to the shared supervisor.

## Dependencies and consumers

The descriptor declares the following dependencies. The native role host and
`server-go/cmd/aimee-module` consume the role implementation and its identity.

- `config`: shared configuration service required by the composition manifest.
- `memory`: shared Go implementation serving the placement's memory operations.
- `module-runtime`: event-bus attachment, immutable identity, and process supervision.
- `postgres`: PostgreSQL provider required before dependent storage operations succeed.
- `vault`: resource-host bootstrap and credential storage used by the composition.

## Providers and readiness

`supervisor.Read` validates the entire manifest before launching any child. It
requires this role, config, PostgreSQL, and memory, rejects duplicate entries and
the opposite role, and verifies executable paths. Workers wait for the owning
bus socket before attachment. Manifest validation alone does not prove each
module's downstream service is ready; service health must also be checked.

## Configuration and activation

- `runtime_toggle.supported`: `false`; the role is selected by the immutable instance identity.

`AIMEE_HOME` identifies the instance home. The deployment supplies the bus socket
and generated placement manifest to `Supervise`. A composition must match the
persisted role; changing KB connection settings cannot switch that role. The
Server and KB role modules never attach together to one composition.

## Surfaces

The role's public surface is the identity event `12801` and the generated
process manifest consumed by its supervisor. User-facing HTTP, CLI, and browser
surfaces remain implemented by the application adapters and their owning
modules; the identity handler exposes no role-editing endpoint.

## Data and migrations

First boot writes a synced, read-only `instance-identity.json` in the instance
home. Subsequent boots must match its role and UUID. Preserve the complete home
in backups and restore it with the corresponding storage and Vault state.
Storage schema changes belong to PostgreSQL and memory; this module provides
no automatic conversion between a Server home and a KB home.

## Security and privacy

The core bus remains authoritative for executable identity and grants. It
rejects a conflicting role even if a grant names that principal and refuses a
second live process for the same role. Corrupt or writable
`instance-identity.json` records fail startup. This startup contract does not
protect against an administrator replacing files on disk.

## Supported journeys

A KB-free Server keeps personal memories through the local Go memory and PostgreSQL modules. Connecting an optional KB adds shared-memory access without changing the Server identity. Ordinary Docker storage is the default; the explicit LUKS
Compose overlay is a PostgreSQL deployment option, independent of `server`
identity. See `docs/DEPLOYMENT.md` for the deployment topology and storage choices.

## Tests and failure behavior

`server-go/modules/server/role_test.go` checks identity responses, malformed or
mutating requests, and conflicting roles. The shared identity and supervisor
suites cover persisted identity and composition failure. A failed manifest
never partially activates; failed module processes restart independently.
The deployment matrix exercises role identity with real published containers.

## Operational diagnostics

Supervisor logs use `composition:server` and identify each child start, exit,
and restart. Inspect those logs alongside bus-attachment and module health
errors. Check the identity file and manifest before investigating dependent
services when startup reports an identity mismatch or a missing required module.

## Compatibility

The native protocol/resource hosts remain packaged for existing C adapters;
the Go `server` module supplies selection and lifetime. Existing module event
identities and the identity response schema remain stable. Recreating a
container with the same persistent instance home must retain its UUID and role.

## Extension and removal

Extend composition through descriptor-declared modules and the validated
manifest, retaining the bus grant checks. Do not add an identity mutation to
`NewHandler` or enable both roles in one manifest. Removing this role from its
own composition fails the required-module check; retire its principal identity
rather than reusing the number for another module.
