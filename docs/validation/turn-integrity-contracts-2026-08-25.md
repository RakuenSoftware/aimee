# Turn-integrity contracts: clean-guest validation

## Scope

Validation covered the complete integration branch, including turn manifests,
context authority and freshness, effect contracts, retrieval continuations,
checker failure policy, and benchmark manifests. The original working tree was
not used for build or deployment.

## Environment

- Proxmox host: `root@192.168.1.252`, `pve-manager/9.2.6`
- Fresh guest: CT 9104, `aimee-turn-integrity`, Debian 13
- Guest address: `192.168.0.217`
- Resources: 6 vCPU, 12 GiB RAM, 2 GiB swap, 32 GiB disk
- Source transfer: Git bundle, checked out detached at
  `8d05bb283ac5f05740d8fc4b343ea94561b1e358`
- PostgreSQL: 17.11, with `vector` and `pg_trgm`
- Final database: freshly created `aimee_turn_integrity_final`

The guest was created with `pct create` from
`debian-13-standard_13.6-1_amd64.tar.zst`; no existing Aimee guest or service was
used. CT 9104 is intentionally left running for inspection.

## Full-stack result

`scripts/aimee-local-stack-e2e.sh --mode full` ran the real `aimee-server`,
`aimee-kb`, server DB1/config/Postgres modules, and KB config/Postgres modules.
The final run reported:

- top-level full-stack suite: **6 passed, 0 failed**;
- nested persistent write/read suite: **3 passed, 0 failed**;
- first-user claim and thin-client certificate enrollment succeeded;
- bearer-only mutation was denied before certificate presentation;
- mTLS advanced from optional enrollment to application-required;
- server and KB version endpoints reported `8d05bb283`;
- server-to-KB status and search succeeded;
- a memory was stored, listed, found by keyword search, and remained present in
  PostgreSQL;
- pgvector was ready and the write path did not report a dimension mismatch.

The KB health document correctly reported degraded synthesis capacity because
the clean guest had no external embedder or synthesis model configured. The
server-facing KB contract remained available with pgvector status `ok`. This is
an explicit capacity verdict, not a hidden fallback.

## Exploratory probes

While the green stack was held open:

- unauthenticated `GET /v1/health` returned HTTP 401;
- authenticated server health returned `status=ok`;
- all seven service/module processes were live;
- two `curator.invalidated` calls advanced both scoped and global knowledge
  epochs from 1 to 2;
- the WORM ledger contained four corresponding `knowledge.invalidated` rows
  (scoped and global for each call), at sequences 175, 176, 181, and 182;
- an empty KB query returned a typed zero-hit response;
- a malformed KB search returned HTTP 400;
- the final database contained the persisted memory row;
- no module attach denial remained after startup.

The WORM implementation's independent test also passed append-only triggers,
cross-store determinism, tamper detection, checkpoint binding, and sealed-snapshot
verification. Immutable filesystem flags are unavailable in the unprivileged
LXC, so the seal was cryptographic-only, as reported by the test.

## Focused regression suite in CT 9104

All of these passed against the final commit:

- `unit-test-turn-integrity`
- `unit-test-td-search-render`
- `unit-test-roundtable-pipeline-eval`
- `unit-test-memory-retrieval-eval`
- `unit-test-process-module-handlers`
- `unit-test-mcp-native-dispatch`
- `unit-test-agent`
- `unit-test-audit-worm`

The MCP test's two federated-KB cases were skipped because that unit process was
not given `AIMEE_KB_API_URL`; the same server-to-KB surface passed in the live
full-stack run. Agent tests that explicitly require `AIMEE_STORE_URL` were also
skipped by that unit binary; DB1/Postgres operation was exercised by the live
stack.

## Failure-path discoveries

The clean guest exposed two validation-state problems rather than feature-code
failures:

1. The base image lacked Go, required to build the event-bus fixture. Installing
   Go 1.24 allowed the pinned module dependencies and toolchain to build.
2. The local full-stack harness had drifted behind the DB1 module boundary: it
   omitted the server-side Postgres module and the narrow DB1-to-Postgres client
   grant. The event bus correctly refused the undeclared edge, and TLS correctly
   stayed disabled while DB1 PKI was unavailable. The harness now declares and
   starts that dependency explicitly.

A repeated scratch-home run also confirmed that first-user enrollment is durable
in PostgreSQL. The final run therefore used a new database instead of deleting or
reusing the prior successful run's identity state.
