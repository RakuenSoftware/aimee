# Private correction proposal and review validation

The Go memory owner now persists private model correction drafts outside serving
memory. Draft identity binds the collection owner, target ID/revision and screened
payload digest. Repeated suggestions reuse pending and rejected drafts. Inspection
is bounded and checks the surviving private parent; deletion cascades to draft
payloads.

Authenticated exact-draft approval or rejection produces an immutable durable
decision. Approval retains the model's principal, transport and confidence ceiling,
records the user reviewer separately and keeps the private memory ID stable.
Canonical content, authorship, retained history, invalidation and the final
proposal decision share a transaction. A review retry verifies current result
eligibility/version before returning the original decision. Retired or erased
results cannot be released by replay.

`POST /v1/memory/correction_proposals` and
`POST /v1/memory/review_correction` accept explicit `store=user`. Their default
remains `kb`, preserving the existing shared command meaning. Host adapters only
route complete envelopes and verified context; the memory module and its bus
producer/consumer remain Go, and the C bus is unchanged. Private store/supersede
refusals now include a linked proposal reference. Private ordinary mutation
idempotency keys remain unsupported and are still acceptance work.

Migration 30 also prevents a model upsert from clearing an elapsed validity
interval. Review metadata is covered by revision/history/invalidation triggers;
ordinary read accounting remains outside those content triggers. Runtime roles
can insert drafts and update only decision columns under the durable guard, but
cannot rewrite payloads, terminal decisions or erase draft rows directly.

## Local evidence

The full uncached memory and Aimee-family race suites pass with both required
PostgreSQL fixtures enabled (51.653 seconds and 1.413 seconds). The new restricted
runtime-role fixture checks persisted proposals despite canonical refusal,
deduplication, inspection, unchanged serving data, model review refusal, digest
mismatch, rollback after a late decision-write failure, preserved model and human
history authorship, retry without another canonical effect, rejected-draft
suppression, actual concurrent reviewer blocking, revocation and erasure.

Native Server forwarding and dispatch tests pass, including verified versus
unverified user context and exact decimal identifiers. Memory ownership,
C-boundary, schema synchronization, Server API conformance, route coverage,
CLI transport routing and dispatch capability checks pass. The latest pushed
predecessor `62fab510ea` completed full CI successfully (run 35525951584).

## Fresh deployment evidence

Application and harness `d66c9bb860` passed **572/572** checks in new owned
deployments on `.253`, CT 9498:

- T2: **396/396** (135 private, 208 shared, 29 shared correction-review,
  six identity and 18 topology checks).
- T3: **176/176** (135 private, 15 real-model semantic, 21 exploratory and
  five topology checks).

The sanitized [T2 receipt](memory-shared-reliability-2026-09-20/fresh-t2-d66c9bb860.json)
and [T3 receipt](memory-shared-reliability-2026-09-20/fresh-t3-d66c9bb860.json)
contain only check names and booleans. Each private run adds 32 review checks:
linked MCP drafts, unchanged canonical state, deduplication, exact inspection,
wrong-digest refusal, rollback on a late decision-write failure, preserved model
origin and human reviewer, retained human history, replay without another effect,
rejected-draft suppression, Server restart, retirement and physical erasure.
The existing semantic outage/expiry and exploratory concurrent Unicode writes,
exact int64 transport, Go-owner suspension/termination and supervised recovery
checks pass too. These runs do not establish a new whole-request P95 claim.

All nine containers were independently checked against the intended image IDs:

- Application: `sha256:fb1f71f1fb9c106bc427ba5831e2ad3fd498fc8a88cdbb5252a1d005340f9fac`.
- PostgreSQL: `sha256:b6209cde68c9a7a65c562b8a4ca45682f138b4a2de5b5dbcfe7ca04ec48e962f`.
- Embedder: `sha256:f1286af7de10cf058a9bec14c45326d64de73e3878db19c732db86cda1f9d979`.

Raw evidence is retained under `/opt/aimee-memory-proposals-evidence/` in
`t2-d66c9bb860-fresh` and `t3-d66c9bb860-fresh`. Containers are stopped; volumes
and evidence remain. An earlier T2 attempt failed during second-stack creation
because retained fixture networks exhausted Docker's default address pools.
Removing the recent owned stopped fixtures' containers/networks restored capacity;
the successful runs used new projects and stores with unchanged application code.

Full [CI run 35527251440](https://github.com/RakuenSoftware/aimee/actions/runs/35527251440)
passed on `d66c9bb860`, including native sanitizers, packaged Go/PostgreSQL replay
and every deployment topology, including encrypted storage and upgrade/rollback.
Benchmark Smoke, Release policy and C repository pin checks passed on the same
head. The evidence/documentation follow-up does not change the tested code.

The MR-01–MR-18 acceptance matrix remains open; this change does not claim
completion of MR-02's remaining mutation verbs, private keyed retries, consumer
checkpointing, retention/restore or full freshness requirements.
