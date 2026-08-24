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

## Default-on activation evidence

The default-on change was independently rebuilt and deployed on the requested `.252` host in a
second isolated environment. The final source snapshot passed the complete temporal acceptance
runner on that host: 24 benchmark-schema cases, the 12-case golden memory-sufficiency gate,
typed-fact, curator, mining, DB2, and schema-ordering suites. Focused configuration, KB-client,
ingress, and gateway tests also passed.

Live requests omitted every temporal enable flag. They proved current semantic assertions, active
observations, and reviewed procedures were enabled and packed by default, while historical
assertions, episodes, summaries, and working context remained disabled. Separate requests proved
the master opt-out, every temporal channel opt-out, malformed-flag fail-closed behavior, and the
explicit historical opt-in.

An isolated capture provider then exercised normal `aimee-server` prompt assembly under the
repository's default strict code-context mode. The provider received the temporal-learning header,
the untrusted memory boundary, the reviewed-procedure boundary, and representative assertion,
observation, and procedure rows. A second request with `X-Aimee-Preinject: 0` contained none of
those bytes, proving the immediate ingress rollback control.

Raw activation evidence is retained under:

```text
/root/aimee-temporal-default-on-20260824T194144Z/results/
  final-acceptance.log
  build-and-focused-tests.log
  live-smoke.log
  typed-context-default.json
  typed-context-master-off.json
  typed-context-channel-opt-outs.json
  typed-context-malformed-flag.json
  typed-context-historical-opt-in.json
  captured-provider-request-final.json
  captured-provider-request-optout.json
  e2e-provider-assertions.json
  deployment-status.txt
```

At report time, the isolated validation deployment remains active on loopback ports `18880`
(`aimee-server`), `18881` (`aimee-kb`), and `18882` (the capture provider). Its dedicated database
and role are both named `aimee_ton_20260824`; it does not replace the earlier deployment.

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
