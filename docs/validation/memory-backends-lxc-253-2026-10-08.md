# Fresh LXC memory-backend validation on .253 — 2026-10-08

**Release verdict: not qualified for full interchangeable Aimee support.** These are actual fresh guests and actual shipping APIs, not mocked backend tests. Baseline checks pass, but the expanded acceptance suite exposes blocking Aimee defects. This report does not claim 100% correctness, full Discord answer-quality validation, or production-scale qualification.

## Native follow-up

[Native repair follow-up](native-memory-fixes-2026-10-08.md) fixes the shared author-erasure selection, recovery admission timeout handling and isolated evaluation cleanup. CT9212 now passes full required native replay, 12/12 fresh shipping baseline, actual-author erasure and full reboot persistence checks. The results below remain the original acceptance record. Contract compliance applies only to external integrations; native is evaluated against its own intended behavior. External shipping recall/briefing/export/admission gaps remain pending.

## Inventory and reproducibility

Host: `192.168.1.253`. Fresh Debian 13 guests, independently provisioned disks, private credentials and separate Aimee identities. Existing CT100, CT101, CT9210 and Discord CT9211 were left running and unchanged. Bot-to-bot conversation and unlimited turns were not restricted.

| Guest | Backend | Resources | Provider |
|---|---|---|---|
| 9212 (`192.168.0.158`) | Native | 4 cores, 8 GiB RAM, 32 GiB disk | PostgreSQL 18.6 + pgvector |
| 9213 (`192.168.0.18`) | Cognee | 4 cores, 12 GiB RAM, 40 GiB disk | Actual `cognee[api]==1.6.2`; deterministic local model/embedding fixture |
| 9214 (`192.168.0.146`) | Hillock | 4 cores, 8 GiB RAM, 32 GiB disk | Actual pinned Hillock `1edd166ead75b85a9ab95cd6ba4faf7011ad567c`, NumPy 2.2.6 |

Go 1.26.7; actual C server/KB and Go process modules compiled inside each guest. All three received the same working-tree source bundle (base `0b298d23c`, not a pristine commit), initial archive SHA256 `6e61e51c84d6ce0610dbdf32db7f9c862e40df42bcd9b2ea3d1145f31d02537e`. Recorded subsequent patches enable external `server-search` and explicitly create UTF-8 isolated evaluation databases. Evidence includes source and executable hashes. The evaluation patch affects test-created databases; running C daemons were built before that patch.

[Guest harness](../../scripts/validation/memory/lxc/README.md). [Fixture-only machine-readable evidence](memory-backends-lxc-253-2026-10-08/). Credentials, certificates, Vault files, private environment snapshots and provider accounts are excluded. Original failure evidence is retained alongside reruns. Providers listen on guest loopback; the enrolled client uses mTLS. The stack is installed as `aimee-memory-validation.service`; guest autostart on the Proxmox host remains disabled. Runtime homes are retained with an exact-path tmpfiles cleanup exclusion.

## Shipping API results

| Acceptance | Native | Cognee | Hillock |
|---|---|---|---|
| Baseline shipping checks | **12/12** | **14/16** | **14/16** |
| Private/shared CRUD, exact `69cm`, provenance text, correction/history, stale-version refusal, retries, project/placement isolation, deletion | Pass | Pass | Pass |
| Memory child restart retains both placements | Pass | Pass | Pass |
| Chatbot recall `/v1/memory/recall` | Pass | **Fail: unavailable** | **Fail: unavailable** |
| Shared context assembly `/v1/memory/read` | Pass | Pass (baseline text assembly) | Pass (baseline text assembly) |
| Verified public `memory.backend_export` | Not an implemented native portable export | **Fail: host context lost** | **Fail: host context lost** |
| Shipping KB briefing `/v1/actions/memory.briefing` | Pass | **Fail: module unavailable** | **Fail: module unavailable** |
| Native record-table access revoked during external CRUD/search | N/A | Pass | Pass |
| Provider stop/recovery, exact reads continue, search fails explicitly | N/A | Pass | Pass |
| 257 eligible records: explicit capacity refusal, all records remain durable | N/A | Pass | Pass |
| Actual-author physical erasure | **Fail: shared authored record survives** | Pass | Pass |
| Stale records restored with current erasure metadata retained | Not qualified by this test | Pass: purged before readiness | Pass: purged before readiness |
| Fresh authorized write after erasure | Not tested | **Fail: permanent author tombstone** | **Fail: permanent author tombstone** |

Baseline erasure uses an unrelated subject and does not establish actual-author cleanup. The extended test intentionally verifies actual durable authorship. Native returned `coverage_complete=true` with zero deleted memories but retained the shared record (`owner_principal=''`, `memory_fact_actors.actor_principal='owner'`). Private native fixture content was erased; the shared content survived a memory child restart. This is an Aimee erasure-selection defect, not a Hillock defect.

External erasure removes authored content, history and receipt payloads. Restoration protection works when current `erasures.json` is retained. The same control permanently rejects later writes by the erased author, returning a misleading expected-version conflict. Introduce a host-admitted subject epoch: invalidate old writers/restores while allowing explicitly admitted new consent. Do not simply remove the tombstone or permit stale replay.

The export handlers have unit-level verified-authority enforcement, but the public generic command dispatcher does not forward trusted host context. Fix the shipping invocation boundary; do not accept an authority field supplied in the request body. Full portable migration, including native records and rich lineage, remains pending.

## Full suites and fixture corrections

Original race and required PostgreSQL owner-replay runs failed. C ownership/boundary and module egress checks passed on every guest. Required standalone live Cognee and Hillock contracts both passed; each runner starts the actual provider and requires the live Go test to execute.

Two setup issues were separated from product failures:

- The disposable login lacked schema `USAGE`. Granting only `USAGE` on the test schema repairs its visibility without granting forbidden mutation permissions.
- Debian's test cluster template0 used SQL_ASCII. Aimee's evaluation owner inherited it, then failed its UTF-8 hashing bootstrap. The evaluation owner now explicitly creates UTF-8 isolated databases with compatible `C` locale; the isolation regression checks `server_encoding`.

The original native runtime replay also exceeded the caller deadline on graph fusion under PostgreSQL's default JIT configuration. Reruns preserve those failures and set `jit=off` only for the disposable test administrator, with the schema fix and UTF-8 patch. The rerun succeeded past the original graph timeout, but changing configuration and concurrent load together does not isolate its cause or establish production latency. Evidence-recovery deadlines also failed under concurrent load in native regression runs: reservation at 25 ms and, on the native guest rerun, initial canonical recovery at its 2 s ceiling. A separate five-run focused recovery test on CT9213 passed all five; this is load-sensitive and not a deterministic failure in every run. The real PostgreSQL UTF-8/isolation/cancellation tests passed in the same focused run. This suite exercises native PostgreSQL semantics on all guests; passing it does not provide those extended features to external adapters.

Final rerun stage results (a failing stage is not hidden by the harness process exiting normally):

| Guest | Broad race/backend suite | Required native owner/eval replay | Real provider contract | C boundaries/egress |
|---|---|---|---|---|
| 9212 native | Fail: recovery deadline | Fail: recovery deadline and evaluation DB cleanup | N/A | Pass |
| 9213 Cognee | Fail: recovery deadline | Pass | Pass | Pass |
| 9214 Hillock | Pass | Fail: recovery deadline and evaluation DB cleanup | Pass | Pass |

The native guest's corpus replay and the Hillock guest's native fidelity test left their randomly named evaluation databases after `Close()` failed its 15 s cleanup deadline. Their fixture test bodies do not establish successful cleanup. Retain this as an Aimee evaluation-owner cleanup/load qualification issue; these failures do not originate in Hillock ranking or Cognee. The focused isolated-database tests passed, so universal cleanup failure is not established. Parsed test statuses preserve all failures and skips; optional unconfigured live-provider tests inside the generic suite are covered separately by the required real-provider runners.

All three fresh guests passed full reboot: enrolled mTLS health, both selected-backend listings and unchanged retained erasure controls. Additional post-reboot probes verified the server certificate against the enrolled CA, without `curl -k`; their results are in `qualification-verified-tls/summary.json`. The original baseline probes skipped server certificate verification while supplying enrolled client certificates; the added strict probes close that transport check for health/listing, not a complete TLS security audit. Guest reboot outcomes are retained in each backend's `qualification-guest-reboot/summary.json`.

## Hillock: specific changes and ownership

### Must-have for full Aimee support

1. **Aimee adapter parity:** implement source-revalidated chatbot recall and KB briefing through backend-neutral contracts, including active policy/control context, typed claims, lineage and current/historical eligibility. Public CRUD alone is insufficient. Cognee has the same missing adapter work.
2. **Trusted migration routes:** carry verified host context to export/import, provide native ↔ external migration, preserve stable identities, revision history, scopes, lineage and erasure controls, then test public round trips. This belongs to Aimee, not Hillock's ranker.
3. **Correct erasure/admission:** repair native authored-record selection and external post-erasure admission epochs, while retaining stale-writer and restore protection. This belongs to Aimee and its compatibility catalog.
4. **Hillock scale contract:** expose bounded retrieval with stable IDs and exact revisions for a declared representative corpus beyond 256 eligible records. Add paginated/snapshot-consistent retrieval or a generation-bound persistent index. Publish limits, cancellation, deadlines, overload outcomes and performance measurements. Current refusal is correct but prevents larger-corpus support.
5. **If Hillock's graph/extraction becomes canonical:** expose immutable per-assertion source IDs, message/document offsets, authenticated speaker identity, entity type, units, negation/uncertainty, correction/conflict/history and deterministic scoped enumeration. The pinned upstream relation key overwrites original `source_doc` on repetition and name-part matching joins person/mountain candidates. Do not enable it as authoritative storage before fixing these semantics.
6. **If Hillock keeps derived state:** provide audience/node namespaces, revision/generation fencing, idempotent mutations and verified scoped cleanup across graph, reservoirs and associations, including startup/restore and late-writer checks. Avoid demo reseeding into application memory. Current stateless sidecar avoids these risks and does not require a persistent graph.

### Nice-to-have

- Pinned semantic/GloVe profile for paraphrases, measured against answer-level fixtures.
- TALON extraction as source-bound proposals requiring Aimee admission.
- Multi-hop reasoning with explainable source/revision paths.
- Opt-in scoped Hebbian ranking with evaluation and rollback.
- Async indexing only if needed to meet the declared workload.

Hillock's own chat renderer is unnecessary for backend support; Aimee owns replies. The sidecar uses real upstream subword/HYDRA ranking and avoids the pinned upstream's broken non-streaming greeting branch.

### Actual ranking diagnostic

With separately typed person `69cm` and mountain `69 feet` fixture records, Hillock ranked the person first for “Kibukx real height” and “Who told you Kibukx height?”, and the mountain for “The mountain elevation”. It also retrieved the positive height record for a negated query, demonstrating that retrieval score cannot resolve truth or contradiction. The unrelated lexical-disjoint query returned no hits. These are bounded ranking observations, not completed conversational disambiguation or general semantic-quality tests. See [raw language probes](memory-backends-lxc-253-2026-10-08/hillock/extended-evidence/hillock-language-probes.json).

No real inference or Discord messages were generated in this validation. Full answer-level replay of the original multi-speaker transcript, remote model quality, live Discord routing, large-corpus latency, disk-full/power-loss behavior, multi-node replication and native-to-external migration remain unqualified. The existing [Hillock gap assessment](../../integrations/hillock/CAPABILITY_GAP_ASSESSMENT.md) and [pending replacement proposal](../proposals/pending/external-memory-engine-scale-and-quality.md) retain these requirements.
