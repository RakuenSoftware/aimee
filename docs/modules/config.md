# config module

## Purpose and non-goals

`config` owns defaults, YAML parsing, validation, optimistic versions, and atomic persistence for
editable server settings. It does not own environment deployment policy, Vault secret values, domain
databases, or arbitrary filesystem access by consumers. Callers use its pure-Go contract.

## Public contracts

Principal `2` serves event `4609`, stage `1`, with the versioned operation catalog in
`src/modules/config/eventcontract/operations.json`. Bounded JSON requests cover reads, snapshots,
versioned writes, defaults, trigger rules, workspaces, and atomic multi-field mutations.

## Dependencies and consumers

- `module-runtime`: authenticates principal `2`, bounds calls, supervises lifecycle, and reports readiness.

Most server modules consume effective values or snapshots through `server-go/config`. CLI and browser
settings surfaces call the same mutation contract, so validation and version conflicts behave identically.

## Providers and readiness

The pinned external package `github.com/RakuenSoftware/aimee-module-config` provides the implementation.
Readiness requires a readable, valid configuration plus a writable parent when persistence is needed.
Malformed YAML, invalid defaults, or an unsafe path prevents ready state rather than loading partial values.

## Configuration and activation

- `runtime_toggle.supported`: `false`; configuration is a required substrate and cannot be disabled while dependent modules are running.

`AIMEE_CONFIG_PATH` selects the file; otherwise the module uses `$AIMEE_HOME/aimee.yaml` and then the
platform config directory. This path controls configuration only: DB1 is PostgreSQL and is not stored there.

## Surfaces

Operators use `aimee config show`, `get`, and `set`, plus the browser settings page. Internal consumers
use snapshots and typed accessors. Structured objects remain file-authored or use dedicated atomic
operations; no caller receives the backing path, file descriptor, or unredacted secret value.

## Data and migrations

The durable artifact is canonical `aimee.yaml`, written through temporary-file, sync, and rename steps.
Schema evolution happens through validated defaults and explicit key retirement, not ad hoc rewrites.
Workflow definitions live under `$AIMEE_HOME/workflows`; DB1 and DB2 remain separate PostgreSQL stores.

## Security and privacy

Secret-shaped settings expose configured state or redacted values and route writes to Vault-backed
surfaces where declared. Paths are resolved beneath approved roots, file permissions are restrictive,
and logs omit credentials. Optimistic `version` checks prevent one actor from silently overwriting
another actor's reviewed configuration update or replacing a newer complete snapshot.

## Supported journeys

On startup the module parses one file, applies defaults, validates the complete tree, and publishes a
stable version. An operator reads a key, submits `set-versioned`, and receives either an atomic new
version or a conflict. Consumers request consistent snapshots instead of racing independent reads.

## Tests and failure behavior

The external package and repository contract fixtures cover parsing, defaults, bounds, atomic writes,
conflicts, malformed JSON, and operation vectors. Unknown keys, trailing JSON, invalid values, oversized
replies, unsafe paths, and failed `set-versioned` fsyncs return typed errors; the old file remains intact.

## Operational diagnostics

Use module readiness, the config path, validation error, operation name, and version hash to diagnose
failures. `aimee config show` confirms effective non-secret values. Logs identify the rejected key and
constraint without printing credentials, raw secret-bearing YAML, or unrelated configuration sections.

## Compatibility

Principal `2`, event `4609`, operation names, JSON field meanings, and version hashes are stable contracts.
Retired 0.2 and mid-cycle keys are not aliases in 0.4.0; use the mappings in `docs/UPGRADING.md` and
regenerate the reference from canonical metadata rather than documenting dead spellings as active.

## Extension and removal

Add a `config` key in the external canonical metadata with type, default, validation, redaction, generated help,
and round-trip tests. Add multi-field changes as atomic operations. Removing a key requires upgrade
documentation and generated-reference cleanup; removing the module requires replacing every consumer.
