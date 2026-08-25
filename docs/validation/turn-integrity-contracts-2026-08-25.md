# Turn-integrity contracts: clean-guest validation

## Scope

This validation covers the integrated turn-integrity work: explicit turn and
effect lifecycles, freshness authority, typed retrieval outcomes, bounded
failure behavior, immutable evaluation manifests, external-effect uncertainty,
and recovery after daemon or dependency loss. The original working tree was not
used for build or deployment.

## Environment

- Proxmox host: `root@192.168.1.252`, `pve-manager/9.2.6`
- Fresh guest: unprivileged CT 9104 (`aimee-turn-integrity`), Debian 13
- Guest address: `192.168.0.217`
- Resources: 6 vCPU, 12 GiB RAM, 2 GiB swap, 32 GiB disk
- Source transfer: Git bundles into `/opt/aimee`, checked out detached
- Product code under full-stack test: `ac85c674f3ee1597fb883954b5a40e338c70fa51`
- PostgreSQL: 17.11 with `vector` and `pg_trgm`
- Final full-stack database: freshly created `aimee_ti_final10`
- Final focused-test checkout: `ca4832f7f8f02e268054ad876710fe012fab19a4`

CT 9104 was created from the Debian 13 standard template for this validation;
no existing Aimee service was used. The CT is intentionally retained for
inspection.

## Real embedding service

The final run required a real embedding service and refused degraded/hash-only
operation. Its health contract reported:

```json
{
  "status": "ok",
  "dim": 1024,
  "model": "Qwen3-Embedding-0.6B",
  "serving_id": "Qwen3-Embedding-0.6B/pooling=last/prefix=none/dim=1024"
}
```

The model was Qwen3-Embedding-0.6B Q8_0 served by `llama-server` on CPU. The KB
stored a marker-bearing fact, then recalled it with a lexically disjoint natural
language query through the ranked semantic memory-search surface. The final
database retained 109 `memory_embeddings` rows and reported no dimension
mismatch.

## Full-stack result

The final command ran `scripts/aimee-local-stack-e2e.sh --mode full` with the
real `aimee-server`, `aimee-kb`, all required server process modules, both KB
process modules, the real embedder, a deterministic model provider, and a real
stdio MCP peer. It exited 0 and reported:

- outer full-stack suite: **9 passed, 0 failed**;
- deep turn-integrity probe: **11 checks passed**;
- nested persistent write/read suite: **3 passed, 0 failed**.

The run proved:

- first-user claim, thin-client certificate enrollment, and mTLS-bound writes;
- bearer-only mutation denial;
- live server-to-KB vector status and search;
- a genuinely empty corpus producing a typed `empty` result with an inert,
  unauthorized continuation;
- model-backed `write_file` and `edit_file` with exact readback;
- an authorized `git_push` reaching a disposable bare Git remote;
- a configured server-hosted stdio MCP mutation reaching the peer and timing out
  as `unknown_outcome`, rather than being treated as safely retryable;
- knowledge invalidation appearing as a stale-knowledge instruction on the next
  turn;
- benchmark execution storing immutable dataset and target hashes, harness
  version 2, hardware profile, and seed 4242;
- a WORM lifecycle containing turn, effect, postcondition, freshness, and
  uncertain-outcome events without raw tool arguments;
- a frozen KB producing a bounded typed failure with no unsafe external
  continuation, followed by live server-to-KB recovery;
- memory store/list/keyword retrieval and semantic retrieval; and
- server restart preserving mTLS identity and memory, followed by KB restart
  restoring the server-to-KB search path.

The final database retained 5 memories, 109 embedding rows, 30 execution-trace
rows, and one evaluation result. Its evaluation manifest was:

```text
suite=turn-integrity-eval
success=true
dataset_hash=5a292b0bcbdbf24c028c0eba5f01b9d375c22648988506f1003c30960f0132b8
target_hash=eb61233ec7ada0b0b0c3eaf06182b7a6176d418c003dc74ac4b0e8ad9d66f1aa
harness_version=2
hardware_profile=Linux/x86_64/cpus=6
seed=4242
```

The retained final scratch tree is `/tmp/tmp.ZTzCcOi7VH` in CT 9104. Its WORM
database contains 1,858 rows, including exactly one `unknown_outcome` effect and
two `knowledge.invalidated` events.

## Focused regression suite

All focused binaries compiled and passed inside CT 9104:

- `unit-test-turn-integrity`
- `unit-test-td-search-render`
- `unit-test-roundtable-pipeline-eval`
- `unit-test-memory-retrieval-eval`
- `unit-test-process-module-handlers`
- `unit-test-mcp-native-dispatch`
- `unit-test-agent`
- `unit-test-audit-worm`

`unit-test-agent` used a separately created PostgreSQL database and the real
store process module, so its execution-trace case was not skipped. The audit
suite passed append-only enforcement, cross-store determinism, tamper detection,
checkpoint binding, and sealed-snapshot verification. The unprivileged CT cannot
set immutable filesystem flags, so sealed snapshots correctly reported
cryptographic-only sealing.

The MCP unit binary skipped its two optional federated-KB cases because that
standalone process was not given a live `AIMEE_KB_API_URL`. The corresponding
server-to-KB and server-hosted MCP paths were both exercised successfully by the
full live-stack run.

## Failures discovered by real-environment validation

The iterative clean-guest runs exposed and repaired issues that local or
fixture-only checks had hidden:

1. An empty-retrieval assertion ran after feedback memories existed; it now runs
   before the first corpus write.
2. Semantic evidence used a context-assembly route rather than ranked fact
   search; it now proves lexically disjoint recall through the semantic ranker.
3. Daemon-only restarts did not re-arm supervised process modules; the harness
   now reproduces service-manager behavior and removes stale bus sockets only
   after their owner exits.
4. Trusted-local tool execution did not carry its out-of-band authorization into
   external effect contracts; capability-limited remote callers remain
   unauthorized.
5. The MCP registry trusted a hidden scalar count instead of the documented
   `mcp_clients` array, so a valid server-hosted client was incorrectly routed to
   KB. The array is now authoritative.
6. The optional post-summary hold setting dereferenced an unset variable under
   `set -u`; the final run deliberately left it unset and exited cleanly.
7. A store-backed agent test used a fabricated execution-plan foreign key and
   its focused target omitted the store-module prerequisite. The test now creates
   a real plan and the target builds its required module.

Earlier failed attempts were retained long enough for diagnosis rather than
being reported as acceptance. Only the exit-zero final10 run above is the
full-stack acceptance result.
