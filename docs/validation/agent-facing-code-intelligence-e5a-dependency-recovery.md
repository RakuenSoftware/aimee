# E5a dependency status and recovery validation

- **Date:** 2026-07-30 UTC
- **Slice:** E5a of `agent-facing-code-intelligence-effectiveness.md`
- **Base:** `testing` at `f889a05b9` after the final pre-publication rebase
- **Candidate branch:** `agent/indexing-e5a-dependency-recovery`
- **Environment:** Linux x86_64, GCC with `-Werror`, clang-format 19.1.7

## Contract exercised

The checked-in fixtures cover the dependency boundary independently of deployment policy:

1. Agent-facing retrieval classifies exactly `ok`, `empty`, `abstained`, `stale`, `unavailable`,
   or `unauthorized`; a transport failure or invalid response cannot become an empty result.
2. The KB transport and external embedder have independent process-local breakers. Three
   consecutive transient failures open a breaker with bounded exponential jitter; further calls
   are suppressed until exactly one half-open probe is eligible.
3. A successful half-open probe closes the breaker without a client/process restart. Failed probes
   reopen it with a larger delay capped at 30 seconds.
4. A reachable KB's typed embedder or vector-store outage preserves that dependency identity and
   does not trip the KB transport breaker or suppress unrelated KB routes.
5. External embedder failure and vector-dimension mismatch are distinct from a valid no-answer:
   unavailable returns HTTP 503 and stale returns HTTP 409 with dimension evidence.
6. Agent-facing code and memory readers preserve typed failures instead of rendering them as an
   empty index, `[]`, “not found,” or an abstention.
7. An unavailable first/new-task retrieval rearms only the exact session/project marker. A related
   follow-up can use the breaker's recovery probe, while successful/empty/abstained results retain
   the existing no-repeat behavior.
8. Built-in local embeddings and all local inspection/edit/test paths bypass these optional-service
   breakers.

E5b separately owns process liveness versus retrieval readiness, deployment restart policy,
operator diagnostics, and the recovery runbook. E5c owns infrastructure-invalid benchmark cells
and checkpoint/resume semantics.

## Focused commands and results

```text
make -C src build/obj/tests/unit-test-dependency-breaker \
  build/obj/tests/unit-test-kb-client-search \
  build/obj/tests/unit-test-kb-client-memory \
  build/obj/tests/unit-test-ingress-preinject \
  build/obj/tests/unit-test-memory \
  build/obj/tests/unit-test-memory-embed-http-auth \
  build/obj/tests/unit-test-kb-http-routes -j4

src/build/obj/tests/unit-test-dependency-breaker
src/build/obj/tests/unit-test-kb-client-search
src/build/obj/tests/unit-test-kb-client-memory
src/build/obj/tests/unit-test-ingress-preinject
src/build/obj/tests/unit-test-memory
src/build/obj/tests/unit-test-memory-embed-http-auth
src/build/obj/tests/unit-test-kb-http-routes
```

All seven binaries passed. The dependency fixture proves bounded jitter, suppression, one half-open
probe, increasing retry delay, and recovery. The KB-client fixture proves all six statuses,
unauthorized non-retryability, dependency metadata, transport suppression, recovery, and identical
non-2xx handling for HTTP and mTLS transports. Its mTLS raw-body fixture also proves multipart
content type is preserved without falling through to the URL transport. The KB-memory fixture proves
unreachable and healthy-empty reads remain distinct across an opened breaker. The ingress fixture
proves retry after an unavailable first lookup. The memory fixtures prove the same breaker lifecycle
for an external command embedder and preserve external HTTP 401/403 as unauthorized without
poisoning that breaker. The HTTP fixture proves embedder outage, embedder authorization failure,
vector-store outage, and dimension staleness are not reported as `no_answer`.

## Project verification

```text
make -C src kb server -j4
make -C src lint
make -C src proposal-links-check api-conformance-check v1-method-coverage-check
make -C src -j4 unit-tests TEST_RUN_JOBS=4
python3 benchmarks/code-agent-effectiveness/validate_fixtures.py --verify-sources
git diff --check
```

The shipping KB/server build, complete lint suite, proposal links, documented-endpoint conformance,
and dispatch-method coverage all passed. The complete unit suite passed with four-way execution.
The PostgreSQL RLS isolation gate was not run locally because `AIMEE_TEST_PG_URL` is unset; its
required CI job runs against pgvector on every push. `git diff --check` passed.

## Frozen-diff review trail

The review gate was scoped to E5a. It reviewed implementation and failure semantics, not Ponytail
task quality, and none of its verdicts are benchmark evidence.

- Run `oprun_g6a69bd8011459d99_1785396259_129` converged without approval. It identified that the
  submitted brief exposed the unfinished parent E5/E6 program instead of binding the review to the
  independently mergeable E5a slice; the next review was explicitly scoped to the E5a contract.
- Run `oprun_g6a69bd8011459d99_1785396774_130` converged without approval with two of three seats.
  It identified missing retry-delay metadata and missing vector-dimension evidence at the client
  boundary. Both contracts and their regressions were added.
- Run `oprun_g6a69bd8011459d99_1785397781_131` converged without approval with all three seats. It
  identified valid no-answer classification, bounded retry-delay floors, exact memory-miss versus
  transport-failure behavior, and unconditional vector-status documentation. Those findings were
  incorporated.
- Run `oprun_g6a69bd8011459d99_1785399544_132` converged without approval with two of three seats.
  It found three remaining transport boundaries: mTLS non-2xx bodies could reach result parsers,
  malformed project-list responses could look empty, and external embedder 401/403 could look
  unavailable. mTLS now rejects non-2xx bodies after preserving typed status, the list parser
  requires a real `projects` array, and embedder authorization is a non-retryable unauthorized
  result that does not advance the transient-failure breaker.
- Runs `oprun_g6a69bd8011459d99_1785401243_133` and
  `oprun_g6a69bd8011459d99_1785401479_134` converged without approval because an incorrect HTTP
  artifact field delivered only the review brief, not the frozen diff. They made no code finding
  and were discarded; the subsequent reviews use the transport's actual artifact field.
- Run `oprun_g6a69bd8011459d99_1785403230_135` converged without approval with all three seats. It
  found that raw-body POSTs used by documentation uploads lacked the mTLS branch and could falsely
  advance the KB transport breaker. The helper now preserves the caller's content type over mTLS,
  rejects typed non-2xx responses, and has a fail-fast regression proving it cannot fall through to
  the URL transport in an mTLS-only deployment.
- Run `oprun_g6a69bd8011459d99_1785404088_136` began the closure review but was interrupted by a
  server replacement before producing an aggregate. Replacement run
  `oprun_g6a6b1ed93980fd5d_1785405189_1` reviewed the recovered identical frozen artifact
  (`2e85eb22015472171720d38cbc1efb9880173681c9dd85d639c0d7d148c79520`), converged with two of
  three seats, and approved it with no findings. One participant failed, so the approved aggregate
  is marked degraded.

## Evidence boundary

This slice verifies truthful failure and recovery mechanics; it does not claim a coding-quality
improvement. The interrupted Ponytail artifacts remain historical red evidence and are not used or
spliced into this validation. E6 must run a fresh pinned matrix after E5a–E5c merge.
