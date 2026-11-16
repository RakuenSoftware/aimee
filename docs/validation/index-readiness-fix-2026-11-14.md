# Standalone indexing and SQL readiness fixes — 2026-11-14

Both defects found in the [general qualification](general-e2e-2026-11-14.md)
are fixed and verified on a new disposable LXC on `192.168.1.253`.
The release remains unqualified because the [PII/Vault audit](pii-vault-2026-11-14.md)
still fails, required CI needs to pass, and one intermittent shared-KB refusal
remains unexplained. Full pins, counts and limits are in the
[machine-readable record](index-readiness-fix-2026-11-14.json).

## Changes

Repository indexing now belongs to host-only runtime operation 67, independently
of the retired memory data operation. Migration 58 stores immutable source
snapshots and atomically publishes a complete scan. Superseded, aborted and
retired snapshots remain retained. Database triggers refuse physical deletion,
truncation and modification of source versions. The existing extractors and
source-observer guard remain in force.

Embeddings use the Native provider, verified serving identity and an explicit
extraction policy. Incomplete or mismatched vector generations cannot supply
dense results. Missing embeddings rebuild after provider recovery even when the
Git commit is unchanged. Native Stage 3 now supports the existing metadata and
batch contracts, and the Go SDK accepts actual positive metadata dimensions.
All seven Native executables were rebuilt from private commit `9be71b7` using
Bookworm; the public bundle contains compiled artifacts and provisioning assets.
Its archive and all 79 public fingerprints verify.

Readiness probes actual SQL success. Successful bus delivery of an SQL error can
no longer keep `db1:ok`. The one-second probe cache and background readiness
snapshot remain bounded; readiness requests do not perform database work.

## Live results

- Independent regression: 34/34 assertions passed.
- Full T3: all six aggregate gates passed, including 353 provider-boundary,
  87 Native asynchronous, nine personal-memory and 34 index/outage assertions.
- Fresh complete T2 repeat: 41 aggregate and 480 underlying assertions passed
  (521 reported checks, including aggregate verdicts).
- Real E4B synthesis: Server 19/19 and KB 19/19, including ontology promotion and
  persistence after restart.
- Real PostgreSQL publication/history tests: 2/2; focused private embedding and
  fixed-stage wire tests: 4/4 each. Go module/SDK tests and all 76 lint checks pass.

Both independent and T3 tests prove automatic clone discovery, symbol/caller and
blast-radius lookup, exact Unicode spans, actual semantic retrieval, generation
readiness, changed-commit publication and retained original SQL source history.
Stopping the embedding provider preserves lexical retrieval and permits source
publication; restarting it rebuilds missing vectors at the same commit.

During a real PostgreSQL outage, canonical session creation fails while readiness
returns HTTP 503, `ready:false` and `db1:fail`. Dependency failure was observed
about 2.2 seconds after the failed session request in both runs. Readiness,
session creation and current index access recover after SQL restoration, with
original source history intact. Independent Native memory remains available.

The first T2 run failed before its planned fifth-request outage intervention:
Native source release refused after four provider requests. The fresh complete
repeat passes all 95 asynchronous assertions. The first failure is preserved as
an intermittent concern; its root cause is not established. Harness beforeimages
also remain: the PostgreSQL image does not provide Python, the span response is
nested, and an unchanged caller declaration legitimately retains its old symbol.
No application behavior was relaxed to satisfy those assertions.

## Evidence and cleanup

Runtime sources match `87710a802`; the final driver matches `4d94516ba`.
All 8,773 tracked source/harness files match the recorded guest snapshot.
The tested application image is
`sha256:e56a2305f48bd17735f2245bed5b4ca907a8ba6ae8148329bfe7cec4cacaddbc`.
The build excludes runtime-web and VS Code and uses ordinary PostgreSQL storage.
E2B, LUKS, browser pairing and upgrade gates were not repeated for this patch.

The local evidence directory is `/home/virant/dev/aimee-index-readiness-fix-20261114`.
The archive contains 44 hashed evidence files; all hashes and the final secret
scan pass. Request frames, receipt capabilities, databases and private keys were
excluded. Path/hash scanner false positives were resolved by separating fields,
without adding scanner exceptions.

CT9474 (`aimee-index-ready-fix`, `192.168.0.169`) was unprivileged and has been
deleted. Its Proxmox configuration and root disk are confirmed absent. CT9473
remains running; other workloads were untouched. No merge, publication or
release promotion occurred.
