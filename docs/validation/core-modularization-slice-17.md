# Core modularization slice 17: config and audit documentation

## Diff scope precondition

The slice-start commit is `dd87702c42d4ec2e8ec9167b375043cbc13650cc`. The allowed close diff is
limited to `docs/modules/config.md`, `docs/modules/audit.md`, the documentation-status partition, this
validation record, and the existing cleanup ledger. Production source, descriptors, schemas, generated
artifacts, build graphs, routes, GUI code, checkers, and optional-module documents are excluded.

## Outcome

This slice promotes exactly required-core `config` and `audit` from documentation debt. Six optional
documents remain: `benchmarks`, `control-web`, `governance`, `roundtable`, `runtime-web`, and
`workflows`. Promotion records contracts and distributed physical ownership; it does not claim source
consolidation, optional profile completion, or dynamic liveness proof.

## Config evidence and gaps

`src/modules/config/module.yaml` declares only `module-runtime` and no runtime toggle. Owned physical
source is `src/modules/config/*`; distributed consumers include server startup/loop, CLI, deployment,
modules, webchat, and frontend settings. `legacy_config_read` selects the published snapshot after server
startup and otherwise reads disk (`config.c:1077`); `config_load_file` applies defaults, validates, and
uses file identity for its cache (`config.c:1084`). Server startup seeds the snapshot
(`server/server_main.c:185`). `handle_config_set` reads disk, saves, reloads immediately, and returns a
field verdict (`server/server_config.c:67`). The main loop processes SIGHUP and calls
`config_reload_if_changed` each tick (`server/server.c:2376`). This is reload, not server restart.

The typed field table classifies hot, re-appliable, and restart-bound values and assigns each field to
a runtime, deploy, advanced, or dev presentation group. `/v1/config` returns every typed field plus non-runtime
group metadata (`server/server_config.c:13`). `frontend/src/pages/Settings.tsx` hides non-runtime groups
by default but does not filter by live module/provider consumption. `webchat/settings.go` maintains a
static settings allowlist. Therefore the approved GUI truthfulness sentence in `config.md` is normative:
dynamic consumer filtering is `not present`; no behavior is changed in this documentation-only slice.

The monolithic flat `legacy_config_record`, hand-maintained field metadata, distributed section parsers/savers, and
separate GUI lists are future consolidation/liveness subjects. A field with only parser, serializer,
default, schema, or self-test evidence is a `configuration-only` candidate, not confirmed dead; proving
removal requires non-test references, dynamic registration, API/GUI exposure, environment readers,
round-trip compatibility, and supported-journey evidence.

## Audit evidence and guarantee matrix

`src/modules/audit/module.yaml` declares `config`, `module-runtime`, and `vault` with no runtime toggle.
Physical source comprises action hashing/preview, the legacy ledger reader, SQLite WORM storage, and the
shared WORM chain. Consumers include guardrails, server management/state, trajectory export, KB vault
operations, and `src/modules/db2/c/kb_audit_worm.c`.

| Property | Current evidence | Limit |
|---|---|---|
| Legacy append log | `log.c:219` appends and flushes JSON lines; `log.c:200` rotates by size | no hash chain, retention policy, or immutable storage |
| SQLite append-only | `audit_worm.c:32` creates update/delete rejection triggers | a writer with database/file authority can remove triggers or replace the file |
| Commit durability | `audit_worm.c:103` selects WAL and `:104` sets `synchronous=FULL` | not remote replication, hardware durability, or retention |
| Tamper evidence | `audit_worm_chain.c` hashes fixed length-prefixed fields and prior hash | detects after verification; it does not prevent file replacement or rollback alone |
| Checkpoint attestation | `audit_worm_checkpoint` uses a dedicated keyed MAC | an uncheckpointed intact tail is amber; key custody and external anchoring remain separate |
| Sealed snapshot | `audit_worm_seal` checkpoints, `VACUUM INTO`, then tries an FS immutable flag | filesystem immutability is best-effort and may be unavailable |
| KB parity | `db2/kb_audit_worm.c` consumes `audit_worm_chain.h` | separate store, gate, authority, and failure behavior; byte parity still merits an end-to-end regression |

Retention/deletion schedules, legal hold, external timestamping, remote replication, hardware-backed
keys, mandatory detail secret scanning, and guaranteed immutable media are `not present`. The legacy
action log is currently authoritative; WORM dual-write is default-off and best-effort, and an append
failure does not change the tool verdict (`modules/guardrails/guardrails_action_audit.c:74`).

## Overlap and liveness findings

| Area | Classification | Disposition |
|---|---|---|
| `config_fields` metadata and frontend/webchat lists | `duplicated-by-adjacent-module` candidate, not confirmed | derive truthful GUI projections from live module/provider consumers in a later source slice |
| Config parse/save pairs and flat `legacy_config_record` | boundary overlap | audit field consumers and round-trip behavior before moving ownership into module source |
| Legacy action log and SQLite WORM | dual-write overlap, not dead code | retain until authority, migration, reader parity, and failure policy are explicit |
| Server SQLite and KB PostgreSQL WORM | storage providers sharing canonical primitives | retain independent stores; add byte-identical end-to-end parity evidence |
| General `audit_log` and governed `audit_action_log` | distinct producer/evidence contracts | retain; compare caller coverage before any consolidation |

No `unreachable`, `superseded`, or `test-only` candidate met the static threshold. The WORM store is
configuration-gated but has non-test CLI/API/producer consumers, so it is not configuration-only dead
code. Runtime liveness and real deployment configuration remain a hypothesis, unverified.

## Deferred cleanup observations

Later audit source work should document the KB build exclusion for the SQLite store, list both consumers
on `audit_worm_chain.h`, prove byte-identical server/KB rows through their storage paths, record the DB2
dependency edge, and compile the shared header standalone. These observations do not authorize source,
build, schema, or test changes in this slice.

## Verification

- `python3 -I -S scripts/check_module_docs.py`
- `python3 -I scripts/tests/test_check_module_docs.py -v`
- `python3 -I -S scripts/check_module_source_ownership.py`
- `python3 -I -S scripts/check_cleanup_ledger.py`
- `python3 -I -S scripts/refactor_baselines.py`
- `make -C src lint`
- close-time changed-path and diff-stat scope checks
- technical-writer review and exact-diff roundtable approval
- feature-branch pull-request CI
