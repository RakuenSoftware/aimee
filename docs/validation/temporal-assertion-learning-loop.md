# Temporal Assertion and Learning Loop: Validation Report

Closes the implementation acceptance criteria in the corresponding completed proposal. The
feature is now default-on after review of the representative benchmark evidence recorded here.
Explicit ingress, assembler, and per-channel opt-outs remain available for rollback.

## Scope

Validation covered dual-axis temporal recall, exact evidence, hybrid lexical/vector retrieval,
bounded graph expansion, deny-dominant scope handling, typed context assembly, application and
outcome attribution, recurrence mining, independent-session thresholds, review-only procedure
generation, rollback, restart durability, authentication, malformed input, and concurrent access.

## Local evidence

- Both shipping service binaries compile and link with warnings treated as errors.
- The feature acceptance runner passes in full, including typed-fact, observation, mining,
  attribution, schema parity, migration ordering, and benchmark-schema checks.
- Twenty-four benchmark-schema tests and the twelve-case temporal/contradiction golden corpus
  pass. The golden gate enforces the committed human-calibration metadata floor.
- The formatter dry run, file-size guard, proposal reconciliation, and `git diff --check` pass for
  the feature-owned changes.
- The repository-wide verifier was also attempted. Its shipping build and linking checks passed,
  but the dirty shared worktree has unrelated lint and unit-link failures outside this feature;
  those are not counted as feature acceptance evidence.

## Isolated deployment evidence

Validation ran on a dedicated database, role, home directory, source tree, result directory, and
ports on the requested remote host. Both the control service and knowledge service were built
from the final source layout and ran concurrently.

The pre-restart run passed 24 checks. It exercised:

- current-only and historical temporal recall, including independent world-time and belief-time;
- exact support and contradiction locators;
- hybrid retrieval and bounded two-hop expansion;
- scope denial and allowed-scope parity;
- typed context channels, packing traces, watermarks, budgets, and trust boundaries;
- atomic stage validation and idempotent application attribution; and
- failed-procedure review generation and rejection rollback.

The final post-restart run passed 15 checks and 64 concurrent mixed-scope requests. It proved:

- both services became healthy and reconnected;
- persisted attribution events and temporal facts survived restart;
- two successful recoveries from two independent sessions became an active observation;
- the recurrence watermark advanced to the last source event;
- recovery learning created exactly one pending review proposal;
- missing and invalid credentials were denied;
- excessive traversal depth and negative metrics failed closed;
- the negative-metric transaction left neither a source event nor an attribution row; and
- unrelated vector neighbors were excluded by the similarity floor.

Raw remote evidence is retained under:

```text
/root/aimee-e2e-temporal-20260824T1018Z/results/
  pre-restart-e2e.json
  final-post-restart-e2e.json
  kb.log
  server.log
```

At report time, both isolated services remain running on loopback ports `18780` and `18781` for
inspection.

## Default-on activation evidence on `testing`

Commit `d7fdec06ca`, based directly on `testing`, was rebuilt and deployed in an isolated
module-based stack on the requested `.252` host. The extracted config module reported
`kb_mining_failure_learning_enabled=1`; both `aimee-kb` and `aimee-server` passed their live health
gates on loopback ports `18931` and `18933`.

The deployed typed-context action received no temporal enable flags. It packed current semantic
assertions, active observations, and reviewed procedures by default while leaving historical
assertions, episodes, summaries, and working context disabled. A second live request set only the
master flag to false and returned zero context with every channel disabled, proving the immediate
rollback control.

The exact deployed snapshot also passed the complete temporal acceptance runner, all Go package
tests, all 68 lint checks, and a full release build on `.252`. The acceptance runner includes the
24-case benchmark schema, the 12-case golden memory-sufficiency gate, typed-fact, curator, mining,
DB2, and schema-ordering coverage.

Raw evidence is retained under:

```text
/root/aimee-temporal-testing-default-on-20260824T210000Z/results/
  source-commit.txt
  build-test-lint.log
  lint-build-retry.log
  isolated-live-deployment.log
  deployment-status.txt
  typed-context-default.json
  typed-context-master-off.json
  server-health.json
  kb-health.json
  server.log
  kb.log
  server-config-module.log
  kb-config-module.log
```

The deployment used a disposable database and was stopped and removed after the assertions; the
earlier isolated validation services were not modified.

## Findings corrected during exploratory testing

1. A vector search initially admitted the entire nearest-neighbor tail. A minimum similarity
   floor now prevents irrelevant vector-only assertions from entering fusion.
2. PostgreSQL exposes booleans as text. Two scheduler fields were being read through a numeric
   parser, making an acquired advisory lock and an enabled job both appear false. Both values are
   now normalized to numeric values in SQL, with regression guards.
3. The expanded context implementation exceeded the repository file-size policy. It now lives in
   a separate context-focused translation unit and passes the size and formatting checks.

## Residual operational note

The knowledge service did not complete graceful shutdown within five seconds in this environment,
so isolated restart tests used a validated exact-PID forced stop. Crash/restart durability passed;
graceful shutdown latency is a broader service-lifecycle concern and does not alter the learning
or retrieval contracts validated here.
