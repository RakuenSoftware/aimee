# MR-18: Frozen evaluation, migration parity and release gates

- **State:** Proposed
- **Priority:** P0: starts before behavior changes and evolves with every proposal
- **Owner:** Evaluation and all affected module owners
- **Depends on:** None for the harness; feature-specific tests activate with each proposal
- **Delivery:** Four implementation slices

## Problem and intended result

Similar benchmark names can conceal different evidence units, budgets, answerability filters and denominators. Endpoint migrations can preserve API names while losing temporal rules, semantic retrieval or parameter behavior. Unit tests alone do not prove that the authenticated transport, database role and final provider request enforce the same contract.

Create one frozen memory-reliability suite, one serving-surface parity matrix and a versioned experiment manifest. Separate deterministic invariants, retrieval quality, end-to-end task quality and operational cost. All gates below are proposed requirements, not claims of already passing implementation tests.

## Harness and manifest

Extend `benchmarks/common/{result_schema.py,runner.py}`, the memory evaluation fixtures and integration tests. Preserve unanswerable cases in dataset loaders; record every exclusion with its reason. Distinguish turn, claim, chunk, session and document retrieval units.

Pin implementation, dataset hash and ordered case IDs, input corpus, answerability labels, model/tokenizer/index generations, ingestion/extractor settings, policy/feature flags, reader/judge configuration, seed, requested/effective candidate limits and final payload budgets. Track missing results, invalid runs, timeouts and infrastructure errors. Historical published values are not reused as measurements of changed implementations.

Use gold labels only in evaluation. No serving planner, source metadata or requirement generator receives expected answer text or gold evidence IDs. Separate a protocol-compatible track from product-optimized ingestion/assembly; keep their scores distinct.

## Frozen adversarial cases

The minimum corpus includes these case groups, each with successful and failing variants:

| Group | Required cases |
|---|---|
| Validity and authority | New correction versus semantically similar old fact; future/expired/superseded/archived/quarantined/deleted records; global preference versus project constraint; model overwrite of user evidence; immutable same-key inserts |
| Time | Conflicting world-time/belief-time; late-recorded correction; exact interval boundaries; historical access after utility horizon; erasure still enforced historically |
| Scope | Cross-user/project retrieval; pooled connection reuse; unauthorized graph intermediates; hidden parent/family metadata; task-fork audience conflicts |
| Independence | Thirty duplicates; summary corroborating its own parent; low-trust source copied into many derivatives; composite A+B counted as a third witness; unknown lineage |
| Retrieval and selection | Dense-only relevant hit after full lexical pool; lexical distractor; misleading graph proximity; capped priors; sole-support displacement; mandatory floor conflict; exposure feedback loop |
| Packing and receipts | Long metadata/Unicode/escapes; duplicate procedures; required evidence dropped at final pack; trace truncation; concurrent traces; crash before admission, after admission before handoff, after send before dispatch logging, and after response before acknowledgement persistence; changed payload; commitment-only replay claim |
| Contracts and task state | Wrong complete/high-confidence plan; starvation/expansion; zero versus disabled budget; concurrent delegate spending; stale contract; temporary hypothesis promotion; expired projection |
| Lifecycle and operations | Same-dimension embedding drift; backlog deadline; delete during backfill; historical semantic recall after rebuild; future-valid activation without a write; restore after erasure; commit-before-publication crash; duplicate/reordered invalidations; consumer restart/retention gap; stale review preview; failed hygiene proposal; rejected proposal repetition |
| Query caches | New constraint after cached briefing; new contradiction after empty result; distinct task/view/time/budget identities; clock-only applicability transition; newly visible record; lagging consumer with unchanged local watermark |
| Go memory ownership | No native files or C descriptor entries in memory; module-side producer and consumer are Go; no relocated C memory behavior; the bus remains C and external C callers remain transport-only; `CGO_ENABLED=0` memory and Go-client builds; same domain decision through CLI/MCP/HTTP/bus adapters; Server/KB placement isolation; unsupported contract version; cancellation/deadline propagation; module restart/unavailability; reject native memory implementations, module-side C adapters and cgo wrappers |
| Effects and retries | Revoke before admission; changed destination; duplicate external write; unknown effect outcome; forbidden action composition; clean retry preserving real effects |
| Injection and answerability | Imported imperative instructions; forged source/authority metadata; same-channel instruction confusion; canary leakage; unanswerable and conflicting-evidence tasks |

## Metrics and definitions

Report hit@k/any-gold success separately from evidence recall and all-required-evidence coverage. MRR measures the first relevant hit. Precision/nDCG require a declared relevance labeling scheme. Equal k does not mean equal model context: compare equal final token/byte budgets and disclose evidence units.

For answerability, publish the confusion matrix, abstention precision/recall, unsupported-answer rate and risk-coverage. No abstentions makes precision undefined, not automatically perfect. False-complete rate uses reviewed requirement satisfaction, separately from final-answer correctness.

Measure task completion/correctness, citation support, raw discovery calls, distinct files/bytes read, contract expansions/false restrictions, repair turns, health metrics, p50/p95/p99 latency, queue lag and total stage-level cost. Separate warm/cold indexes and caches. Report uncertainty, repetitions and missing data.

## Release policy

Hard deterministic gates require zero violations in their frozen cases: unauthorized release, forbidden current-state re-entry, authority laundering, canonical mutation from an unapproved task/hygiene proposal, receipt mismatch, silent embedding-identity mixing and independent-support inflation. Zero fixture violations is not a claim of zero production risk.

Performance/quality policies are frozen before each experiment. Proposed initial canary defaults are: a one-percentage-point noninferiority margin for task success with a paired 95% interval; p95 latency no greater than 1.10 times matched baseline; and no increase in matched total task cost for a change marketed as an efficiency improvement. Exploration-contract promotion additionally targets at least 20% fewer redundant raw discovery calls. These are reviewable release targets, not measured benefits. If the sample is too small to support the decision, keep the feature in observe/canary mode. A tradeoff that misses a threshold requires an explicit revised product decision and a new evaluation, not post-hoc relabeling of the run.

Run ablations in dependency order: eligibility, mutation, packing, fair candidates, sufficiency, independence, priors/diversity, recovery and exploration contracts. Then measure interactions. Tune on train/dev cases, freeze held-out evaluation and check reader/judge sensitivity for model-graded tasks.

## Serving-surface parity

Expand the [initial serving-surface inventory](memory-reliability-00-program.md#initial-serving-surface-inventory) into a checked-in matrix for CLI, MCP, HTTP, event bus, ingress, provider dispatch, scheduled maintenance and compatibility APIs. Columns include owner/store namespace, supported query modes, authenticated scope, dense/graph capability, index state, activation/workspace parameters, final budget and receipt stage. Intentionally unsupported behavior is explicit; ignored advertised parameters fail conformance. Pin source revision and distinguish source inspection from exercised guarantees.

Exercise a real non-owner PostgreSQL role and the current C/Go processes in integration CI. Capture the final provider-shaped request with a controlled fake transport; verify exact selected IDs, protected bytes and receipt binding without requiring a paid model call. Keep separate small live-model quality runs where they materially test reading behavior.

Use deterministic barriers at mutation commit/publication, consumer apply/checkpoint, dispatch admission/handoff and response/acknowledgement persistence. Restart the affected process and assert durable states, current release decisions and replay progress. Missing dispatch evidence after admission must remain unknown; successful publication alone cannot satisfy complete erasure. Advance a controlled clock for future-valid indexing and cache boundaries without modifying source content.

Apply the [Go memory integration contract](memory-reliability-00-program.md#go-memory-module-integration) and G0 replacement gate to every implementation slice. Run Go domain and caller/handler communication tests, `CGO_ENABLED=0` builds of the memory executable and caller tooling, dependency-closure inspection and real process/placement conformance. Port the C framing fixtures into Go and test the supported entry points after their cutover. Change `scripts/check_memory_c_boundary.py` to reject native sources/headers in the memory trees and descriptor, plus relocated memory behavior and module-side C bus adapters elsewhere. Validate module-bus boundaries, Make/CMake registration and installed headers. Negative fixtures must reject moved C memory policy, native code in the memory dependency closure, forwarding headers, module-side callback shims and cgo wrappers. Positive integration fixtures must exercise Go memory producers and consumers against the existing C bus, including permitted external C transport callers. The C bus is not being converted to Go. Keep the immutable whole-repository native inventory as ownership-review evidence; its historical all-callers criterion is broader than this module boundary. Emptying the module directory or compiling only its Go server cannot certify behavior still implemented in native hosts. Rollback keeps a compatible Go memory implementation and module-side communication path or reports the capability unavailable.


## Implementation slices and rollout

1. Add manifest/exclusion/answerability schemas, deterministic fixture groups and metric unit tests; record the current baseline and per-operation Go ownership/migration dispositions from the program inventory.
2. Add cross-surface authenticated integration tests and final-request capture, including failure/restart cases.
3. Add paired quality/cost/routing/contract experiments and sensitivity reports with fixed thresholds.
4. Make applicable invariant tests required in CI, reconcile stale implementation/proposal documentation and publish a versioned release report. Retain raw per-case evidence under its access/retention policy.

Rollback uses the last-known-good behavior/policy while preserving corrected authorization, history and receipt invariants. Optional features must have independent switches; a failed ranking experiment must not require disabling the entire memory correctness layer.

[Program and common contracts](memory-reliability-00-program.md) · [Requirements coverage](memory-reliability-requirements-coverage.md)
