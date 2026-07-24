# audit module

## Purpose and non-goals

`audit` is required core and owns security-relevant event recording, privacy-bounded governed-action
evidence, tamper-evident WORM records, verification, checkpoints, sealed snapshots, and audit-ledger
reads. It does not authorize actions, own general application logging, define retention policy, provide
immutable hardware/storage, or make a best-effort dual write authoritative.

## Public contracts

`audit_args_hash` and `audit_command_preview` produce bounded action evidence;
`audit_ledger_read` reads legacy `tool_action` rows; `audit_worm_append`, `audit_worm_verify`,
`audit_worm_checkpoint`, `audit_worm_seal`, and `audit_worm_read_page` form the server WORM contract.
The four canonical public headers live under `src/modules/audit/include/aimee/audit/`, and consumers
include them as `aimee/audit/<header>.h`. `audit_action.h` owns bounded governed-action evidence;
`audit_ledger.h` owns legacy ledger reads; `audit_worm.h` owns the server store API; and
`audit_worm_chain.h` owns the engine-independent canonical row-hash and checkpoint-MAC primitives.
The KB PostgreSQL store consumes `audit_worm_row_hash` at the shared canonical row-hash boundary.

`src/modules/audit/module.yaml` declares ownership of four production sources, four canonical public
headers, four direct unit tests, and this document; the module has no private headers. Its
`ownership_complete: true` latch exhaustively checks module-local C and private-header files and requires
this canonical document. Public-header and test entries are explicit audited claims rather than
auto-discovered completeness domains. The source liveness, build membership, adjacent-boundary, and
test-facing API audit is recorded in `docs/validation/core-modularization-slice-34.md`.

## Dependencies and consumers

- `config`: supplies the action-audit and WORM capture gates.
- `module-runtime`: supplies required lifecycle and readiness contracts for audit capability.
- `vault`: supplies the custody boundary for audit and checkpoint keys.

Consumers include `execution-policy`/guardrails, server management and vault paths, trajectory export,
audit CLI/API/dashboard readers, KB custody operations, and the separate DB2 KB WORM provider. Producers
own event meaning; audit owns safe record formation, storage, verification, and diagnostic results.

## Providers and readiness

The legacy provider appends JSON lines to rotating `audit.log`. The server WORM provider is SQLite at
`$AIMEE_HOME/audit/worm-live.db`; the KB uses a separate PostgreSQL store while sharing the canonical
row-hash primitive. Readiness reports capture gate, store availability, chain result, checkpoint attestation,
and sealed-file immutability separately. Legacy logging remains authoritative while WORM dual-write is
default-off and best-effort.

## Configuration and activation

- `runtime_toggle.supported`: `false`; audit is required core, while individual capture/storage policies such as `audit_worm_enabled` are configurable.

Governed-action audit is gated by `audit_action_enabled`; WORM dual-write is gated by
`audit_worm_enabled`. Both cached gates are cleared by a config reload re-applier. Disabling a capture
path does not remove audit capability or permit a producer to bypass a mandatory higher-level policy;
the configured authority and loss behavior must remain visible.

## Surfaces

Surfaces include per-tool action emission, general `audit_log` producers, `aimee audit verify`,
checkpoint, seal, and metric commands, management APIs/dashboard log reads, trajectory export, and KB
WORM calls. These surfaces must label legacy versus WORM evidence, server versus KB stores, and
green/amber/red verification without collapsing distinct guarantees into a generic `secure` result.

## Data and migrations

Legacy `audit.log` stores rotated JSON lines. Server WORM rows contain gap-free `seq`, advisory `ts`,
actor, action, subject, verdict, bounded detail, key ID, previous hash, and row hash. SQLite triggers
reject update/delete and `synchronous=FULL` protects commits; the SHA-256 chain detects row mutation,
reordering, and gaps, while keyed checkpoints attest a head. Sealed snapshots use `VACUUM INTO` and a
best-effort filesystem immutable flag.

Retention, deletion schedules, archival lifecycle, remote replication, legal hold, external timestamping,
hardware-backed keys, and guaranteed filesystem immutability are `not present`. Append-only triggers,
`synchronous=FULL` commit synchronization, a hash chain, MAC checkpoints, and a best-effort immutable flag are separate properties
and must not be represented as one stronger WORM guarantee.

## Security and privacy

Tool arguments, commands, principals, subjects, details, and operator-provided timestamps are untrusted
and may contain secrets or personal data. Governed-action hashing uses a per-tool allowlist and dedicated
HMAC key; command previews retain program basenames but no arguments. WORM detail is size-bounded but
per-action secret scanning is `not present`, so producers must provide privacy-safe detail and audit
readers require authorization.

## Supported journeys

After `execution-policy` decides a tool verdict, the wrapper emits exactly one legacy action row and,
when enabled, attempts the corresponding WORM append without changing that verdict. Operators can read
records, verify linkage/checkpoints, add a checkpoint, and seal a point-in-time snapshot. Server and KB
stores produce the same canonical row hashes, but remain independently enabled, stored, and operated.

## Tests and failure behavior

The descriptor owns the four tests that directly exercise audit module implementations. Similarly
named tests remain with their actual subsystem or integration boundary; a filename containing
`audit` is not ownership evidence.

| Test | Boundary exercised | Audit ownership |
| --- | --- | --- |
| `test_audit_action.c` | `audit_args_hash`, key provisioning, HMAC vectors, and bounded command previews from `audit_action.c` | Owned |
| `test_audit_ledger.c` | `audit_ledger_read` parsing, ordering, rotation, filtering, and malformed-input handling from `audit_ledger.c` | Owned |
| `test_audit_worm.c` | SQLite append/read, mutation rejection, checkpoints, sealing, paging, and verification from `audit_worm.c`; consumes the shared chain | Owned |
| `test_audit_worm_chain.c` | Engine-independent row serialization, hashing, hex encoding, and checkpoint MAC invariants from `audit_worm_chain.c` | Owned |
| `test_audit_action_log.c` | The core `log.c` JSON-line writer and `audit_action_log` API | Not assigned; no audit module implementation is under test |
| `test_code_audit.c` | The independent CLI code-audit feature | Not assigned; code-health analysis is not the audit evidence module |
| `test_code_audit_graph.c` | The independent code-audit graph algorithms | Not assigned; code-health analysis is not the audit evidence module |
| `test_db2_code_audit.c` | DB2 code-audit assembly and PostgreSQL query shims | Not assigned; this is a DB2 code-intelligence test |
| `test_harness_memory_audit.c` | Memory-interception JSONL logging through `hmem_audit` | Not assigned; this is a memory harness test |
| `test_kb_audit_worm.c` | The KB PostgreSQL store and artifact capture seam, using the shared canonical row-hash contract | Not assigned; this is a mixed KB/store integration test |
| `test_kb_audit_worm_pg.c` | SQL and C append parity against a real PostgreSQL KB store | Not assigned; this is a mixed KB/PostgreSQL integration test |
| `test_token_audit.c` | DB1 token accounting and agent ingress attribution | Not assigned; this is a DB1/agent-accounting test |
| `test_token_audit_load.c` | Concurrent DB1 token writes and asynchronous ingress recording | Not assigned; this is a DB1/agent-ingress load test |
| `test_vault_audit.c` | Vault-to-server audit emission through the core log | Not assigned; this is a vault/server/log integration test |

The direct tests deliberately separate the shared chain primitive from the SQLite store that consumes
it. That is complementary coverage, not duplicate ownership. The CMake suite registers the action and
ledger tests; the Make unit-test suite registers the WORM store and chain tests. Issue #1753 records the
evidence used to resolve this classification.

Build membership is intentionally not uniform today. Make's `CORE_SRCS` contains all four audit sources;
its KB projection removes only the SQLite-specific `audit_worm.c`, retaining the pure shared chain.
CMake's `CORE_SRCS`, and therefore the `aimee-core` static library plus its server/KB projections, contains
only `audit_action.c` and `audit_ledger.c`. The descriptor records canonical source ownership; it does not
claim that the current Make and CMake products expose identical WORM capabilities. Closing that existing
build-profile gap requires a separate behavior and dependency slice.

Current governed-action emission is best-effort: hashing writes a stable sentinel on failure, and WORM
loss does not block the action while legacy logging is authoritative.

## Operational diagnostics

Report store identity, capture/authority mode, append failure, safe action/verdict metadata, head and
checkpoint sequence, green/amber/red verification, seal path, and whether the kernel immutable flag was
actually set. Exclude raw tool arguments, secrets, private keys, full sensitive details, and an
unbounded principal or subject. `amber` means an intact unattested tail, not corruption.

## Compatibility

The `v1-` args-hash form, per-tool projection, WORM domain and fixed field order, genesis value,
gap-free sequence, checkpoint MAC contract, verification status meanings, event fields, and legacy
reader ordering are compatibility contracts. Server SQLite and KB PostgreSQL must produce byte-identical
canonical row hashes at the shared row-hash seam; storage-engine migrations cannot silently rewrite
historical records. The checkpoint-MAC contract remains a separate compatibility boundary.

## Extension and removal

New producers use bounded typed event schemas; new stores implement the `audit_worm_chain` canonical
contract or declare a versioned migration. Legacy/WORM dual paths, server/KB stores, and general/action emitters are
overlap candidates requiring authority, parity, failure-policy, retention, and reader-liveness evidence.
No path is confirmed dead; consolidation that weakens privacy, durability, or independent verification
is outside a behavior-preserving source move.
