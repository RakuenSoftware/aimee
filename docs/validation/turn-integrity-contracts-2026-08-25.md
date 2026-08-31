# Turn-integrity contracts: real-environment acceptance

## Scope

This validation covers the integrated turn-integrity work: explicit turn and
effect lifecycles, freshness authority, typed retrieval outcomes, bounded
failure behavior, immutable evaluation manifests, external-effect uncertainty,
postcondition enforcement, and recovery after daemon or dependency loss. It
also validates the active embedding contract after retiring the obsolete
embedding fixture and its model-specific assumptions.

The original working tree was not used for build or deployment.

## Environment

- Proxmox host: `root@192.168.1.252`, `pve-manager/9.2.6`
- Fresh guest: privileged CT 9106 (`aimee-turn-integrity-pr`), Debian 13
- Guest address: `192.168.0.8`
- Resources visible in the guest: 8 CPU, 24 GiB RAM, 4 GiB swap, 40 GiB disk
- Source transfer: Git bundles into `/opt/aimee`, checked out detached
- Product and harness revision: `efd730555b8031978c5e0c28b3f2647684633eb0`
- PostgreSQL: 17.11 with `vector` and `pg_trgm`
- Acceptance database: freshly created `aimee_turn_integrity_efd73055_final`
- Retained scratch tree: `/tmp/tmp.7f8adDfIJ2`

CT 9106 was created from the Debian 13 standard template for this validation.
No existing Aimee service, database, or build artifact was used. The CT is
retained for inspection.

## Real embedding service

The acceptance run required a real embedding service and refused degraded or
hash-only operation. The service loaded the registry's shipped default model
through Sentence Transformers and reported:

```json
{
  "status": "ok",
  "model": "bekko-a25m",
  "repo": "hotchpotch/bekko-embedding-v1-a25m",
  "dim": 384,
  "quantize": "fp32",
  "runtime": "torch",
  "threads": 8,
  "serving_id": "bekko-a25m/8721341054416418"
}
```

A direct three-vector sanity probe returned finite 384-dimensional vectors. The
cosine-equivalent dot product for `cerulean lantern` versus `blue light` was
`0.5131567`; the same query versus `database transaction isolation` was
`0.1022024`.

The full stack then stored a marker-bearing fact and recalled it with a
lexically disjoint natural-language query through the ranked semantic memory
surface. The acceptance database retained 122 `memory_embeddings` rows; every
row reported `vector_dims(embedding) = 384` (116 unit vectors and 6 memory
vectors).

The obsolete embedding script, shim, fixture assumptions, pending proposal,
and stale validation evidence were removed. The disposable guest's 665 MiB
cache from the superseded test path was also deleted before acceptance was
recorded. Historical benchmark results and unrelated chat-provider protocol
compatibility remain as provenance and supported behavior; neither participates
in the active embedding configuration.

## Full-stack result

The acceptance command ran `scripts/aimee-local-stack-e2e.sh --mode full` with
freshly built `aimee`, `aimee-server`, `aimee-kb`, all required server and KB
process modules, the real embedder, a deterministic model provider, and a real
stdio MCP peer. Real-embedder enforcement, turn-integrity MCP coverage,
component restart coverage, and scratch retention were all enabled.

It exited 0 and reported:

- outer full-stack suite: **10 passed, 0 failed**;
- deep turn-integrity probe: **11 checks passed**;
- nested persistent write/read suite: **3 passed, 0 failed**.

The run proved:

- first-user claim, thin-client certificate enrollment, and mTLS-bound writes;
- bearer-only mutation denial;
- live server-to-KB vector status and search;
- a genuinely empty corpus producing a typed empty result with an inert
  continuation;
- model-backed `write_file` and `edit_file` with exact readback;
- an authorized `git_push` reaching a disposable bare Git remote;
- a configured stdio MCP mutation timing out as an uncertain outcome rather
  than being treated as safely retryable;
- knowledge invalidation appearing as a stale-knowledge instruction on the next
  turn;
- benchmark execution storing immutable dataset and target identities;
- a bounded WORM lifecycle for turn, effect, postcondition, freshness, and
  uncertain-outcome events without raw tool arguments;
- a stopped KB producing a bounded typed failure, followed by live recovery;
- memory store/list/keyword retrieval and lexically disjoint semantic recall;
- durable memory-governance rejection and scope separation;
- server restart preserving mTLS identity and persisted memory; and
- KB restart restoring the live server-to-KB search path.

## Focused regression and build checks

Before the guest acceptance run, the feature checkout completed:

- native builds of `aimee`, `aimee-server`, `aimee-kb`, `aimee-module`, and
  `aimee-module-config`;
- embedder environment/dynamic-batching tests (10 passed);
- the embedder bake/retry test;
- proposal reconciliation, pending-audit manifest, and documentation checks;
- the complete repository lint gate (70 checks);
- `unit-test-embedding-dim`; and
- focused turn-integrity, agent, IR, memory-stage, MCP dispatch, roundtable, and
  search-render unit binaries.

## Validation incident

The first attempt in CT 9106 stopped during the required Go module build because
the fresh Debian guest did not yet contain the Go toolchain. No runtime checks
were reported from that attempt. Go was installed, a second fresh PostgreSQL
database was created, and only the exit-zero run against that database is the
acceptance result above.
