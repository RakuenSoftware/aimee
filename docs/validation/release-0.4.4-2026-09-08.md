# 0.4.4 restores setup and private indexing on the deployed corpus

The Linux candidate `0.4.4-rc1` is deployed in LXC 103 as a local application image.
It includes the fixes in the image; the earlier binary and GUI hotfix mounts were removed.
The repository release script proposes **0.4.4** after **v0.4.3**. No release tag or public
artifact has been published by this preparation.

## Every registered project published, and restart preserved the data

The candidate published **74 projects and 23,022 files**: all 73 server clones plus the
registered local Aimee checkout. A controlled container restart preserved those counts and
the IDs, keys, and content hashes of both personal memories. The original deployment memory
remained intact; the second memory records the verified published-span and runner contract.
Readiness returned true and the container has no automatic restarts.

[Deployment hashes](release-0.4.4-2026-09-08/deployment.json) identify the image and its server,
thin client, memory/workspace module, and GUI payload. The Linux candidate thin client used
the existing enrolled mTLS identity. [Persistence evidence](release-0.4.4-2026-09-08/persistence.json)
contains counts and memory hashes, without memory contents or credentials.

## Live tests exercised content and recovery

[90 post-restart checks](release-0.4.4-2026-09-08/live-checks.json) passed across six projects:
the local and server Aimee indexes, Wolf, Fenrir, Moonlight Common C, and Moonlight Qt. They
cover symbol lookup, structure, hashed source spans, callers, blast radius, hybrid retrieval,
investigation, missing and sensitive paths, memory retrieval, and 16 concurrent memory
requests. A separate 54-check run passed before restart.

The real GUI authenticated through PAM, displayed all 73 existing clones, and loaded
**Set up this instance** without a server-unavailable response. It then cloned three public
Pallets repositories through the organization flow. Dropping one actual completed response
triggered inventory recovery; each repository was submitted exactly once. All three test
clones and the temporary managed test account were removed. Existing accounts and clones
were preserved. [Browser evidence](release-0.4.4-2026-09-08/browser-checks.json) records the
requests and counts.

## Regressions now run in the release gates

| Validation | Result |
| --- | --- |
| Full local gate | `make verify-local` passed: complete Linux build, lint, unit tests, Go tests, and linkage. |
| Native unit suite | Passed; 637 test binaries scheduled, with environment-dependent skips below. |
| Go unit suite | Passed across all three Go modules, including C/Go bus interoperability. |
| Repository lint | All 77 checks passed. |
| Script suite | All 58 existing script test files passed; the added frontend and upgrade-log gate tests also passed. |
| Frontend | Clean `npm ci`, 209 tests in 26 files, TypeScript, and production build passed. |
| Built-browser regressions | Five scenarios passed: first boot with old dismissal, lost responses and HTTP 503 on both clone screens. |
| PostgreSQL memory gate | Passed with race detection: private code regressions, privacy separation, content gate, and the 105-case retrieval corpus in both Server and KB placement. |
| Workspace race tests | Passed, including never-polled and stale runner admission. |
| Native sanitizers | Collector, source span, module deadlines, and audit hash tests passed ASan/UBSan with leak detection. |

The PostgreSQL gate selects every `TestPrivateCode*` test, including large graphs, missing
files, manifest publication, source bounds, and publication drift. The new frontend CI job
runs unit tests and the built-browser scenarios. Its result is included in the required
`unit-tests` aggregate; a regression proves failed, cancelled, and skipped browser jobs block
that aggregate for code changes.

Run the reusable live check against an enrolled client and existing corpus:

```bash
python3 scripts/test-private-index-live.py --client /path/to/aimee \
  --sample aimee=memory_insert --sample games-on-whales/wolf=die \
  --memory-key aimee-deployment --min-projects 2 --output /tmp/index-checks.json
```

The built-browser regressions run with `npm --prefix frontend run test:browser` after
building the frontend and installing Playwright's pinned Chromium. The recovery fixture holds
publication until the browser observes the checking state, so overlapping inventory polls
cannot race the status assertion. Five consecutive runs of all five scenarios passed.

## Wider validation caught the hidden-file publication failure

Eighteen server clones had no published generation before this candidate. The collector
accepted hidden configuration filenames, including `.mcp.json`, that the private index
rejected. The index also rejected the collector's explicitly supported `.gitmodules`
manifest. One such file aborted the whole scan.

The collector regression failed on the original implementation and passed after aligning
these contracts. Hidden configuration files are excluded; `.gitmodules` remains an explicit
manifest exception. All 18 affected repositories subsequently published on the live instance.

## CI exposed a race in the upgrade test's log assertion

The first CI run passed init/migrate routing but exited 141 at the historical-store test's
final log assertion. Under `pipefail`, `grep -q` closed a matching log stream early and caused
Docker to receive SIGPIPE. The assertion now drains the stream while preserving both missing
message and producer-error failures. A regression with a large stream reproduced exit 141
before the fix and passed all three outcomes afterwards. The complete upgrade test then
passed against disposable PostgreSQL containers for both `aimee_store` and `aimee_shared`.

## Audit recovery retained the original evidence

The original 0.4.3 audit segment contained one invalid final row, sequence 1,152,239. All
preceding 1,152,238 row hashes were independently verified. The original SQLite files and
their hashes remain in `/var/lib/aimee/audit/recovery-20260908-index-repair`; the new segment
begins with an `audit.recovery` record identifying that evidence. No audit verification was
disabled. Golden row hashes and checkpoint MACs remain valid after `OPENSSL_cleanup()`.

## Release review still covers configurations outside this appliance

This live appliance uses standalone Server with its private PostgreSQL and embedder; it has
no shared KB configured. Local native tests skip checks requiring a separate PostgreSQL
store fixture, signed KMS services, tmux, or root-only fixtures. PostgreSQL memory and code
coverage above uses an isolated disposable database, never the appliance's live database.
The final PR commit `bb3fdaa6bb` passed all 58 CI checks, including Windows and macOS builds,
the full sanitizer and script suites, PostgreSQL/workflow gates, and all four container
topologies with encrypted-storage recovery, upgrade, and rollback.
[CI results](release-0.4.4-2026-09-08/ci-final.json) retain each check URL.
PR #2975 merged that tree into `testing` at `ec4717297e`. Protected promotion and release
approval remain; these CI results are separate from the local Linux run described above.

The optional server-side `git_verify` auto-discovery cannot inspect this detached client's
Makefile. Equivalent repository Make targets were run on the client and their results are
recorded above; remote auto-discovery is not claimed as validated by this release check.
