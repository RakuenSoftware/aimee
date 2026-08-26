# Restore dataset-driven direct retrieval benchmarks in Go

- **State:** PENDING. Restored after rejection audit on 2026-08-16.
- **Corrective history:** PR #2662 rejected this objective because its archived implementation
  used a retired C binary and the broken C/SQL scratch store. That was an ownership error, not
  evidence that deterministic LoCoMo and LongMemEval retrieval measurement was obsolete. This
  rewrite assigns suite execution and results to the Go benchmarks owner and consumes memory only
  through its service boundary.
- **Dependency:** The implementation waits for the disposable-run contract in
  [eval-temp-store-schema-relocation.md](eval-temp-store-schema-relocation.md). That proposal is
  pending; this document does not claim the lifecycle exists yet.

## Decision

Restore a dataset-driven, non-LLM retrieval track for LoCoMo and LongMemEval as a Go-owned
benchmark capability.

A bounded CLI under `server-go/cmd` will use a reusable runner in
`server-go/modules/benchmarks`. The runner will parse and validate the datasets, obtain a leased
disposable memory endpoint, ingest each sample, issue direct retrieval requests, calculate the
existing Go MRR/NDCG/recall and latency summaries, and emit versioned structured evidence. Shell
entry points remain thin launchers; they do not own parsing, lifecycle, scoring, or results.

The direct track does not call an agent, answer generator, or judge model. The existing
`benchmarks/targets/aimee/adapter.py` path performs retrieval followed by `agent_run`, so it is an
LLM-assisted track and cannot substitute for this outcome.

`server-go/modules/benchmarks` owns benchmark inputs, execution, scoring, result lifecycle, and
baseline eligibility. Memory owns schema, storage, ingest, retrieval, and deletion semantics. The
runner receives a versioned memory client from the disposable-run lifecycle; it never applies DB2
schema, opens DB2 directly, creates a second database, or falls back to production memory.

This intent and boundary were approved by Aimee review
`roundtable-0ed2b740ca2e7853344d6e2d`.

## Current tree is not coverage

The Aimee direct route is currently advertised but unrunnable:

- `benchmarks/suite/run-direct.sh` dispatches LoCoMo and LongMemEval to
  `benchmarks/locomo/bench_aimee_direct.py` and
  `benchmarks/longmemeval/bench_aimee_direct.py`;
- `benchmarks/embedder-sweep.sh` invokes the same two paths; and
- neither Python entry point exists.

This is not a provisioning-only failure. The removed scripts called the retired
`aimee-client eval <suite>` surface and depended on the legacy scratch-store path. A command that
can only fail with a missing entry point must not be counted as a harness or regression gate.

Coverage that remains is different in kind:

- `bench_aimee_llm.py` exists for both datasets and measures agent/judge behavior;
- the unified target adapter calls an agent after retrieval;
- `memory.benchmark` covers live retrieval suites but not labelled LoCoMo/LongMemEval corpus runs;
  and
- the Go benchmarks module already owns bounded MRR, NDCG, recall, and latency calculations, but
  not dataset parsing or execution.

The missing capability is deterministic labelled-corpus retrieval evidence with no answer model in
the loop.

## Dataset and execution contract

### Corpus parsing

Implement versioned Go parsers for the supported LoCoMo and LongMemEval fixtures. Each parser must:

- preserve stable sample, question, evidence, category, and subset identifiers needed by existing
  result consumers;
- reject malformed JSON, missing required identity/evidence fields, duplicate identities,
  unsupported dataset variants, and inputs above explicit byte/sample/question bounds;
- distinguish an intentionally unanswerable or evidence-free row from malformed input rather than
  silently scoring either as a miss;
- produce deterministic case order independent of map iteration and filesystem enumeration; and
- record the dataset version and digest without copying private corpus content into diagnostics.

Dataset download and licensing remain with the existing provisioning scripts. The runner fails
closed when the requested corpus is missing or its version cannot be established.

### Disposable memory lifecycle

One benchmark run obtains one typed disposable-run lease from the Go lifecycle owned by the
benchmarks module. The leased memory service must prove its run and database identity before ingest.
Each dataset sample uses a fresh namespace or equivalent reset inside that lease, and a later sample
must not retrieve an earlier sample's content.

Cases receive only the run-bound memory client. They do not receive a DSN, schema credentials, or a
DB2 handle. A missing, reused, expired, non-empty, ambiguous, or production-bound lease fails before
corpus ingest. There is no process-global memory or production fallback.

Cancellation stops admission of new cases, finalizes the current run as incomplete, and invokes
only lease-scoped cleanup. Cleanup failure is visible and terminal; it cannot be converted to a
successful benchmark result.

### Retrieval and scoring

For each answerable question, issue direct retrieval against only that sample's ingested corpus and
retain the ordered retrieved identifiers required for relevance scoring. Feed bounded identifier
sets and timing samples through the Go benchmarks scoring and latency contracts. Metric definitions,
K values, skip reasons, and segment aggregation are versioned compatibility fields, not CLI text to
be scraped with regular expressions.

The runner reports retrieval accuracy and latency together. Injected-token accounting may be
included when the memory boundary exposes it deterministically, but its absence must be explicit and
must not fabricate a zero.

## Result lifecycle and baseline safety

Write one complete, schema-validated JSON object per completed case to an append-safe JSONL
artifact. Emit a separate summary by atomic replacement only after the run reaches a terminal state.
Both artifacts carry bounded provenance for suite and dataset version/digest, code revision,
configuration digest, run identity, metric version, sample/case counts, skips, errors, latency, and
cleanup outcome. Secrets, raw credentials, private corpus rows, and production memory content are
excluded.

Existing `benchmarks/common/result_schema.py` consumers are a compatibility boundary. Delivery must
either emit their required direct-track fields or introduce an explicit versioned migration with
consumer tests; it must not silently repurpose an LLM verdict or judge field.

An incomplete, cancelled, unclean, unparsable, or schema-incompatible run can be inspected but can
never update a baseline, satisfy a drift gate, or report a pass. Baseline replacement remains an
explicit operator action after compatibility and completeness checks.

## Legacy C disposition

Nine C dataset functions currently open the legacy temp database. They split into two distinct
outcomes.

The five deterministic direct-retrieval capabilities migrate to this Go track:

- `mem_eval_run_locomo`;
- `mem_eval_run_locomo_session_support`;
- `mem_eval_report_locomo_misses`;
- `mem_eval_run_longmemeval`; and
- `mem_eval_report_longmemeval_misses`.

The four agent/judge QA capabilities are not translated into the non-LLM runner:

- `mem_eval_run_locomo_qa`;
- `mem_eval_report_locomo_qa_failures`;
- `mem_eval_run_longmemeval_qa`; and
- `mem_eval_report_longmemeval_qa_failures`.

Before removing those QA functions, compare their observable behavior with the existing LoCoMo and
LongMemEval `bench_aimee_llm.py` tracks. Proven parity permits retirement; any uncovered outcome
becomes a separate, explicit Go residual with its own owner and acceptance evidence. Neither group
may be deleted merely because the new CLI builds.

Completion requires a mechanical zero-caller gate for the migrated C dataset functions and the
scratch-store retirement identifiers named by the disposable-run proposal. Remove stale shell
dispatch only when the Go launcher replaces it in the same delivery slice; do not leave dead C or a
command that advertises absent coverage.

## Delivery plan

1. Add bounded parsers and canonical case types under `server-go/modules/benchmarks`, with fixture
   tests for both datasets and result-schema compatibility.
2. Add the reusable deterministic runner and a thin `server-go/cmd` CLI. Use fake memory and lease
   implementations for ordering, isolation, cancellation, and failure-state tests.
3. After the disposable-run contract is delivered, add real memory-boundary integration evidence
   for identity, ingest/retrieval, sample isolation, cancellation, and lease-scoped cleanup.
4. Point `benchmarks/suite/run-direct.sh` and the embedder sweep at the Go CLI, preserving supported
   arguments or providing explicit compatibility errors.
5. Establish parity or residual disposition for all nine C dataset functions, enforce the
   zero-caller gate, and then remove their legacy implementations with the scratch-store callers.
6. Only after complete compatible runs exist may cadence or drift-gate work consume this track and
   accept a new baseline.

## Corrective PR acceptance

This documentation-only correction is complete when:

- this proposal exists under `pending/` and the rejected copy is absent;
- the rejection audit records #2662 as the current atomic correction and retains explicit contracts
  for every later misrejection;
- dependent proposals and shell comments describe the missing direct entry points truthfully;
- proposal links, pending-manifest integrity, history ordering, reconciliation, shell syntax, Go
  benchmark tests, and repository lint pass; and
- no executable benchmark behavior or unimplemented delivery evidence is claimed by this PR.

## Future implementation acceptance

These are proposed-new delivery gates. This documentation-only corrective PR does not claim that
the commands, files, or tests exist or pass yet:

~~~yaml acceptance
- {id: 1, tier: unit, check: "cd server-go && go test ./modules/benchmarks -run TestParseLoCoMoRejectsMalformedOversizedAndDuplicateIDs"}
- {id: 2, tier: unit, check: "cd server-go && go test ./modules/benchmarks -run TestParseLongMemEvalRejectsMalformedOversizedAndDuplicateIDs"}
- {id: 3, tier: unit, check: "cd server-go && go test ./modules/benchmarks -run TestDatasetRunnerDeterministicOrderAndSampleIsolation"}
- {id: 4, tier: unit, check: "cd server-go && go test ./modules/benchmarks -run TestDatasetRunnerCancellationFinalizesIncomplete"}
- {id: 5, tier: unit, check: "cd server-go && go test ./modules/benchmarks -run TestIncompleteDatasetRunCannotUpdateBaseline"}
- {id: 6, tier: compatibility, check: "cd server-go && go test ./modules/benchmarks -run TestDatasetResultSchemaCompatibility"}
- {id: 7, tier: integration, check: "cd server-go && go test ./modules/benchmarks -run TestDatasetRunnerUsesDisposableMemoryBoundary"}
- {id: 8, tier: mechanical, check: "future zero-caller gate for the legacy dataset and scratch-store identifiers"}
- {id: 9, tier: privacy, check: "cd server-go && go test ./modules/benchmarks -run TestDatasetResultsExcludePrivateCorpusFields"}
~~~

Completion additionally requires:

- LoCoMo and LongMemEval direct runs are deterministic and make no agent or judge calls;
- malformed, duplicate, oversized, missing, and unsupported corpora fail explicitly;
- sample isolation and run/database identity are proven across success, cancellation, and failure;
- per-case JSONL and the atomic summary pass the declared compatibility contract;
- incomplete or unclean runs are mechanically ineligible for baseline updates;
- fixture-only private-field tokens do not appear in per-case JSONL, summaries, or diagnostics;
- every one of the five direct C capabilities has Go parity and zero callers;
- every one of the four QA capabilities has parity evidence or a named Go residual; and
- the shell entry points invoke the delivered Go CLI rather than absent Python scripts.

## Non-goals

- Restoring the retired `aimee-client` or a C `aimee-kb --eval` entry point.
- Giving benchmarks schema, SQL, DB2, embedding, ranking, or production-memory ownership.
- Running answer generation or judge models in the direct track.
- Implementing the disposable-run lifecycle in this proposal.
- Creating a scheduled cadence, changing baseline policy, or claiming full-dataset CI coverage.
- Removing any C function before parity, caller, compatibility, and isolation evidence exists.
