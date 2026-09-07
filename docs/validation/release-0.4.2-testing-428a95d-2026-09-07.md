# 0.4.2 published testing qualification on .253

**Verdict: not ready to release.** The broad fresh-environment qualification passed,
but promotion CI exposed an intermittent personal-recall failure. A controlled database
contention experiment reproduces that failure on the published image. The Server memory
data deadline repair and its regressions accompany this report. Main-only source-pin and
DB2 boundary checks also require resolution before promotion.

## Exact candidate and fresh environment

Candidate: `428a95deba37b9ad3341d4461e94e01d52ffefc9` (merged #2966).
[Testing publication](https://github.com/RakuenSoftware/aimee/actions/runs/34141726886)
succeeded before deployment. The application and PostgreSQL `:testing` image IDs matched
`:testing-428a95d`. The [image digest records](release-0.4.2-testing-428a95d-2026-09-07/core-images.json)
and [browser model images](release-0.4.2-testing-428a95d-2026-09-07/browser-images.json)
pin the artifacts actually exercised.

VM **9435**, `aimee-testing-428a95d-042`, was created on **192.168.1.253** from Debian 13
cloud media with 8 vCPUs, 24 GB RAM, and a 120 GB disk. Its address is 192.168.0.113.
[Fresh-environment evidence](release-0.4.2-testing-428a95d-2026-09-07/fresh-environment.json)
records empty Docker images, containers, and volumes before deployment.
[Source verification](release-0.4.2-testing-428a95d-2026-09-07/source-verification.json)
checked all 6,893 archived candidate files by SHA-256 with no mismatch.
The initial qualification used published images without binary overlays.

The browser used Debian Chromium 152 and Playwright 1.55.0. Only the disposable guest's
loopback address exposes the browser listener. The retained managed stack has a permanent
validator account; credentials, screenshots, and detailed logs remain private on the VM.
Other existing environments on .253 were not repurposed.

## Completed published-image checks

Each linked JSON contains named verdicts; HTTP response bodies and credential-bearing
fields are excluded. Counts below are assertions, not independent user journeys.

| Area | Result |
| --- | --- |
| T1, standalone KB | [12 topology checks](release-0.4.2-testing-428a95d-2026-09-07/T1/topology.json) passed |
| T2, separate Server and optional KB | [17 topology](release-0.4.2-testing-428a95d-2026-09-07/T2/topology.json), [49 personal-memory](release-0.4.2-testing-428a95d-2026-09-07/T2/local-memory.json), [138 shared-memory](release-0.4.2-testing-428a95d-2026-09-07/T2/shared-memory.json), and [6 identity](release-0.4.2-testing-428a95d-2026-09-07/T2/identity.json) checks passed |
| T3, KB-free Server with local embedding | [4 topology](release-0.4.2-testing-428a95d-2026-09-07/T3/topology.json), [49 personal-memory](release-0.4.2-testing-428a95d-2026-09-07/T3/local-memory.json), and [15 semantic-memory](release-0.4.2-testing-428a95d-2026-09-07/T3/semantic-memory.json) checks passed |
| Actual published 0.4.1 upgrade and rollback | [12 checks](release-0.4.2-testing-428a95d-2026-09-07/upgrade041/results.json) passed, including Unicode canaries, new writes, container recreation, original cluster preservation, and rollback reads |
| Encrypted PostgreSQL | [11 fresh-install](release-0.4.2-testing-428a95d-2026-09-07/luks-fresh.json) and [13 migration/recovery](release-0.4.2-testing-428a95d-2026-09-07/luks-upgrade.json) checks passed |
| Browser setup | [6 checks](release-0.4.2-testing-428a95d-2026-09-07/browser/setup/result.json) passed, including replacement of the bootstrap account |
| Browser navigation | [15 pages](release-0.4.2-testing-428a95d-2026-09-07/navigation.json) passed; desktop and 390-pixel mobile Settings screenshots inspected |
| Local models | [Retirement/restoration](release-0.4.2-testing-428a95d-2026-09-07/model-lifecycle.json) passed; three further isolated browser cycles passed; [real browser-to-E2B inference](release-0.4.2-testing-428a95d-2026-09-07/live-model-browser.json) passed again after final recreation |
| E2B/E4B portability and identity | [8 checks](release-0.4.2-testing-428a95d-2026-09-07/model-probes.json) passed across host and emulated Nehalem CPU modes, managed mTLS discovery/inference, and anonymous refusal |
| Providers | [9 lifecycle](release-0.4.2-testing-428a95d-2026-09-07/browser/providers/exercise.json), [4 restart/deletion](release-0.4.2-testing-428a95d-2026-09-07/browser/providers/after-restart.json), and [5 negative-path](release-0.4.2-testing-428a95d-2026-09-07/browser/providers/exploratory.json) checks passed |
| Provider failure and recovery | [Both GUI pages report module loss](release-0.4.2-testing-428a95d-2026-09-07/browser/providers/module-down.json); [packaged supervision recovers service](release-0.4.2-testing-428a95d-2026-09-07/provider-recovery.json) |
| Exploratory HTTP and published CLI | [79 checks](release-0.4.2-testing-428a95d-2026-09-07/exploratory.json) passed: concurrency, exact Unicode persistence, confidence validation, store isolation, missing IDs, database outage, retirement, and supersede/read behavior |
| Container replacement | [New container, same published image, healthy service and permanent login](release-0.4.2-testing-428a95d-2026-09-07/container-recreation.json) passed; [all four retained services](release-0.4.2-testing-428a95d-2026-09-07/final-runtime.json) matched published image identities and were healthy |
| Supporting regressions | [191 frontend tests and production build, 26 published thin-client tests with installed Codex required and no skips, native synthesis mTLS unit](release-0.4.2-testing-428a95d-2026-09-07/regressions.json) passed |

These results establish broad working coverage; the later contention failure below prevents
an unconditional release verdict.

## Personal recall failure and repair

The candidate's [push CI](https://github.com/RakuenSoftware/aimee/actions/runs/34141726880)
passed 52 jobs with one expected skip. The subsequent
[promotion CI](https://github.com/RakuenSoftware/aimee/actions/runs/34144121755)
failed T2: explicit user-store recall returned HTTP 502, `user memory module unavailable`.
The corresponding canary assertion failed too. The other 47 personal-memory assertions
passed; this was not a blanket failure to start the memory process.

A 1.5-second exclusive lock on the disposable `user_memories` table reproduced the same
[502 on the published application after **0.579 seconds**](release-0.4.2-testing-428a95d-2026-09-07/recall-delay-published.json). Server passed the generic
500 ms stage deadline to the memory data stage. KB and the shared memory data adapter
already allow five seconds for database work. The repair gives Server's memory data stage
the same bounded five-second budget while retaining 500 ms for other stages.

Three additional ordinary T2 deployments also passed on the published image, confirming
that an unloaded repetition alone would miss the contention defect.

The native regression fails against the original caller and passes with the repair. It
models 1.5 seconds of work, checks forced user scope and borrowed-request preservation,
and verifies that expired/unavailable calls still fail. The deployment regression adds
real PostgreSQL contention and requires both a successful recall and the exact canary
with its user-scoped handle. It does not retry a failed recall into success.

The [local repair image](release-0.4.2-testing-428a95d-2026-09-07/memory-repair-image.json)
uses the published application as its base and replaces only `aimee-server`, compiled
with the production Bookworm build dependencies and tree-sitter enabled. It was not
published. The original managed stack remains on the unmodified published image.

[Both repair deployment matrices passed](release-0.4.2-testing-428a95d-2026-09-07/memory-repair-runtime.json):
T2 passed 17 topology, **51 personal-memory**, 138 shared-memory, and six identity checks;
T3 passed four topology, **51 personal-memory**, and 15 semantic-memory checks. Each
personal-memory run includes the real contention regression. T2 took 363.8 seconds and
T3 took 140.3 seconds. [Local validation](release-0.4.2-testing-428a95d-2026-09-07/repair-validation.json)
also passed all 77 lint checks, native deadline and server dispatch regressions, test
registration, formatting, and 22 DB2 boundary tests. A newly published testing image
containing this repair remains necessary before release qualification can close.

## Promotion checks still outstanding

[Draft promotion PR #2967](https://github.com/RakuenSoftware/aimee/pull/2967) exercises
`testing` into `main` with release publishing disabled. Its
[release build preflight](https://github.com/RakuenSoftware/aimee/actions/runs/34144121924)
passed all application/support-image architecture builds, all four thin-client platform
builds, and existing model promotion-source checks. The protected approval job remains
waiting. No release approval or merge was performed.

- The three private includes used by the already merged fact-recall and memory-list tests
  were missing from the DB2 checker's exact test admissions. This patch adds those three
  entries; comparison against `origin/main` and all 22 boundary tests pass, including
  rejection of count growth, production classification, other paths, and other headers.
- The DB2 link-closure comparison rejects the 18 C memory units retired by merged commit
  `3d48beb23b`. Diagnostic review also identifies three retired support units and nine
  newly external memory dependencies. These are consequences of the merged Go memory
  migration. This patch does not waive that ratchet or alter the recorded closure contract.
- The public-surface release baseline is stale as expected on `testing`.
  [The review inventory](release-0.4.2-testing-428a95d-2026-09-07/release-surface-diff.json)
  lists 5 added, 1 removed, and 22 changed public headers, plus route, CLI, configuration,
  schema, and package changes. The release freeze has not been performed on this
  integration repair branch. A [candidate snapshot](release-0.4.2-testing-428a95d-2026-09-07/candidate-surface-baseline.json)
  is included for release review and verifies against the current surface; it does not
  replace `tests/baselines/refactor/index.json`.
- `c-repository-pins` rejects the vendored core/source digest against the existing published
  source lock. Core/package version and lock remain at the existing baseline. Resolving
  the application's coupling to independent repository publication requires an explicit
  release-policy decision; no module repository was created or published during this
  qualification.

The GitHub token cannot rerun failed workflow jobs (HTTP 403). A subsequent repair PR
must run CI normally, and its newly published image needs qualification before release.

## Harness corrections and reproduction

The [harness sources](release-0.4.2-testing-428a95d-2026-09-07/harness) are supplied alongside
the verdicts. They expect the archived candidate at `/opt/validation042/source`, the
published client at `/opt/validation042/aimee-client`, Playwright under `browser/`, and
mode-0700 `private/` and `evidence/` directories. Published image mappings are the recorded
JSON files. The managed driver creates credentials privately. `run-runtime.py` invokes
the repository's T1/T2/T3, real 0.4.1 upgrade, and LUKS harnesses; `run-browser-full.py`
composes the setup, models, provider, CLI/HTTP, recovery, and recreation checks. Existing
provider fixtures come from `scripts/validation/providers/` in the candidate archive.
These scripts restart and stop disposable services and must not target an installation.

Initial harness failures were investigated and retained privately:

- The setup runner inspected the account step before it rendered. It now waits for the
  mandatory account submission control, and all six setup assertions passed.
- Chromium reported `net::ERR_NETWORK_CHANGED` when Docker changed guest network
  interfaces during model lifecycle operations. The browser adapter joins the application
  container's network namespace, leaving Chromium 152 and HTTP assertions intact. Three
  consecutive retirement/restoration cycles then passed.
- The provider recovery probe used `/v1/providers`; the production route is
  `/v1/provider/connections`. After correcting the probe, the full stop/error/kill/supervision
  recovery scenario passed in 33.7 seconds.
- The CLI supersede harness expected a status envelope instead of the returned replacement
  record. It now verifies that record and reads it back for exact content; all 79 assertions
  passed.

These harness corrections are separate from the reproduced application deadline defect.
