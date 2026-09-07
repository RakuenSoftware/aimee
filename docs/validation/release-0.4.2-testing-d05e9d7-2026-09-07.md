# Published testing d05e9d7 qualification for 0.4.2 — NO GO

Follow-up: [upgrade repair and source-publication status](release-0.4.2-upgrade-repair-2026-09-07.md). This report remains the verdict for the original published digest.

The published `testing-d05e9d7` candidate passes fresh deployment and synthesis
qualification, but **must not be promoted to 0.4.2 yet**. A real published 0.4.1 KB
cannot complete the documented upgrade into the unified deployment. The release
repository lock also remains stale. Green candidate CI does not clear either gate.

Candidate: `d05e9d71c258f6ca53b89ba8e1752bf910678dae`, the merge of PR #2964.
[Application publication](https://github.com/RakuenSoftware/aimee/actions/runs/34108283225),
[synthesis publication](https://github.com/RakuenSoftware/aimee/actions/runs/34108283189)
and [complete candidate CI](https://github.com/RakuenSoftware/aimee/actions/runs/34108283229)
all completed successfully. `testing` still named this commit at the final branch check.

## Fresh environment and artifact identity

A new Debian 13 VM, **9434** (`aimee-testing-d05e9d7-042`), was created on
`192.168.1.253`: 8 host-model vCPUs, 24 GiB RAM, 120 GiB disk, guest address
`192.168.0.160`. Existing guests and production deployments were not used as fixtures.
All deployment tests used isolated Docker projects inside this VM.

The application, PostgreSQL, embedding and both synthesis images were pulled from
GHCR. [Exact digests](release-0.4.2-testing-d05e9d7-2026-09-07/images.json)
identify the tested artifacts. Application and PostgreSQL `:testing` identities
matched their `:testing-d05e9d7` tags. Both synthesis `:testing` images matched
their new `:runtime-b10219` identities. No application or image binary was patched
or replaced for these results.

All [6,829 source files](release-0.4.2-testing-d05e9d7-2026-09-07/source-verification.json)
on the guest matched the exact candidate Git archive. Frontend verification and
the native C mTLS diagnostic were built from that archive; the deployed services
remained the published images. The separately published Linux x86-64 thin client
came from this candidate's publication-run artifact.

## Passing qualification

| Gate | Result |
| --- | --- |
| [T1: independent KB](release-0.4.2-testing-d05e9d7-2026-09-07/T1/topology.json) | 12/12 topology checks, including direct confidence rejection and real embedding retrieval |
| [T2: separate Server and KB](release-0.4.2-testing-d05e9d7-2026-09-07/T2/topology.json) | 17/17 topology checks; 49 personal-memory, 138 connected-memory and 6 identity checks |
| [T3: KB-free Server](release-0.4.2-testing-d05e9d7-2026-09-07/T3/topology.json) | 4/4 topology checks; 49 personal-memory and 15 real semantic-memory checks |
| [Fresh encrypted PostgreSQL](release-0.4.2-testing-d05e9d7-2026-09-07/luks-fresh.json) | 11/11, including restart, WAL/crash recovery, missing/corrupt Vault and ciphertext preservation |
| [Standard PostgreSQL 18 plaintext migration](release-0.4.2-testing-d05e9d7-2026-09-07/luks-upgrade.json) | 13/13; this fixture already uses the expected directory and database conventions |
| [E2B synthesis](release-0.4.2-testing-d05e9d7-2026-09-07/model-probes-E2B.json) | Host and no-AVX Nehalem probes, native mTLS discovery/inference, anonymous-client refusal, continued service operation |
| [E4B synthesis](release-0.4.2-testing-d05e9d7-2026-09-07/model-probes-E4B.json) | The same complete image and live-model checks passed |
| [Fresh browser setup](release-0.4.2-testing-d05e9d7-2026-09-07/browser-setup.json) | 6/6: real login, bootstrap-account replacement, keyless local primary, model deployment and optional KB Settings |
| [Model lifecycle](release-0.4.2-testing-d05e9d7-2026-09-07/model-lifecycle.json) | Local model retirement and restoration passed |
| [Published browser/server model execution](release-0.4.2-testing-d05e9d7-2026-09-07/live-model-browser.json) | Real E2B inference through the shipped provider worker passed |
| [Provider browser exercise](release-0.4.2-testing-d05e9d7-2026-09-07/providers/exercise.json) | 9 checks; discovery, edits, two accounts at one endpoint, credential rotation and cancellation |
| [Provider restart](release-0.4.2-testing-d05e9d7-2026-09-07/providers/after-restart.json) | 4 checks; persistence, credential use, model associations and deletion |
| [Provider negative paths](release-0.4.2-testing-d05e9d7-2026-09-07/providers/exploratory.json) | 5 checks; cross-origin refusal, unavailable discovery, incompatible edits and deleted-secret isolation |
| [Container replacement](release-0.4.2-testing-d05e9d7-2026-09-07/container-recreation.json) | New container, same published image, permanent account retained and browser navigation passed |
| [GUI navigation](release-0.4.2-testing-d05e9d7-2026-09-07/navigation.json) | 15 pages rendered without JavaScript errors; desktop and mobile screenshots inspected |
| [Exploratory HTTP and published CLI](release-0.4.2-testing-d05e9d7-2026-09-07/exploratory-reviewed.json) | 78 reviewed checks passed |
| [Local regressions](release-0.4.2-testing-d05e9d7-2026-09-07/local-regressions.json) | 191 frontend tests and production build; 26 published-client tests with installed Codex required, no skips; native mTLS unit gate |
| [Candidate CI](release-0.4.2-testing-d05e9d7-2026-09-07/ci.json) | Complete native, real PostgreSQL, Go, script, sanitizer, static-analysis, secret-scan, build and integration jobs passed |

Exploration included 24 concurrent Unicode/multiline writes with unique IDs and
exact read-back, persistence after restart, confidence boundaries and malformed
values, missing/large IDs, store selection, retirement, and explicit errors plus
recovery during a real database outage. The published CLI completed a
store → get → supersede → get sequence.

The provider process was suspended in the disposable deployment. The recorded
[GUI outage case](release-0.4.2-testing-d05e9d7-2026-09-07/providers/module-down.json)
returned errors instead of an empty successful roster. A repeated suspension
produced a browser fetch failure while the request was blocked; it is not counted
as an additional passing HTTP-status assertion. After terminating the stalled
process, [packaged supervision restored providers](release-0.4.2-testing-d05e9d7-2026-09-07/provider-recovery.json).

## Release blocker: the real 0.4.1 KB upgrade does not complete

A separate fixture ran the published image
`ghcr.io/rakuensoftware/aimee-kb-a25m@sha256:13790bc5ec075cfdb25e9dc3c7719706e25da7445c8febbb988eb0aae5585f46`.
Its HTTP API stored a global and a project-scoped Unicode canary. Its actual
PostgreSQL 18 cluster and application home were stopped and copied for migration;
the original remained available for rollback. Copied application files were
adjusted from the old image's UID 999 to the new image's UID 1000 before the
migration probes. Each follow-up used a separate disposable copy.

The following failures were reproduced in sequence:

1. **The documented mount layout is incomplete.** `docs/UPGRADING.md` says to
   mount the old cluster directory at `/mnt/aimee-postgres-legacy`. The migration
   implementation appends `/pgdata` to that path. The real old KB cluster has
   `PG_VERSION` directly under `$AIMEE_HOME/postgres`. The literal documented
   mount is rejected as `unrecognized legacy data; migration required`.
   [Initial result](release-0.4.2-testing-d05e9d7-2026-09-07/upgrade041/results.json).
2. **The old KB database has a different name.** With the directory layout
   corrected, the new bootstrap looks for `aimee_store`; the old KB has
   `aimee_shared`. It reports that no supported administrative role exists.
   An independent copy confirms that the `aimee` superuser exists and the actual
   failure is `database "aimee_store" does not exist`.
   [Database diagnostic](release-0.4.2-testing-d05e9d7-2026-09-07/upgrade-database-diagnostic.log).
3. **The old Vault DSNs need an explicit migration.** First-boot credential
   bootstrap preserves existing records. After renaming the copied database,
   supplying the normal new Compose inputs does not replace the old local-socket
   DB2/store DSNs. An additional probe explicitly applied the new DSNs with the
   existing one-shot `AIMEE_VAULT_ENV_OVERWRITE=1` control. This is an explicit
   test correction, not something the default upgrade performs.
4. **Migrated PostgreSQL listens only on localhost.** After those corrections,
   the PostgreSQL container becomes healthy, but its separate KB gets TCP
   connection refusal. `SHOW listen_addresses` returns `localhost`; the health
   check tests only container loopback. The retained old configuration is not
   reconciled for the new two-container deployment.
   [Listener and intact-canary evidence](release-0.4.2-testing-d05e9d7-2026-09-07/upgrade-listener-diagnostic.log).
5. **Function ownership prevents schema migration.** Changing the copied
   PostgreSQL listener to `*` and restarting permits the new libpq connection,
   but KB startup now fails with:

   ```text
   aimee: db2_init: schema apply failed: ERROR: must be owner of function pg_now_text
   ```

   The existing `pg_now_text` overloads still belong to `aimee`, while the new
   schema initializer connects as `aimee_store_migrator`.
   `scripts/postgres-store-init.sh` transfers tables, views and standalone
   sequences, but does not transfer these functions. Both canary rows remain
   present in the migrated database while the KB retries and stays unavailable.
   [Ownership evidence](release-0.4.2-testing-d05e9d7-2026-09-07/upgrade-function-ownership.log),
   [schema error](release-0.4.2-testing-d05e9d7-2026-09-07/upgrade-schema-errors.log).

These are migration/procedure failures, not evidence of lost rows. Restarting the
untouched original 0.4.1 deployment successfully read both canaries through its
HTTP API. [Rollback verification](release-0.4.2-testing-d05e9d7-2026-09-07/upgrade-rollback.json).

Release requires a tested end-to-end 0.4.1 migration procedure and regression
coverage using the actual legacy KB layout, names, Vault state and schema objects.
The passing synthetic PostgreSQL migration fixture does not cover that contract.
No workaround above is presented as a completed or supported migration.

## Release blocker: repository provenance

`python3 scripts/check_c_repository_lock.py` still fails with
`core vendored mirror differs from its repository pin`.
[Recorded result](release-0.4.2-testing-d05e9d7-2026-09-07/repository-pins.log).
The release lock needs legitimate published repository commits matching the
vendored sources. Testing-branch CI passing does not satisfy the main/release-only
provenance requirement. No lock values or release tags were changed here.

## Test qualifications and reproduction

The checked-in setup browser script assumed the permanent account already
existed. On this genuinely fresh VM, it stopped at the mandatory account step.
A test-only extension completed account replacement through the actual GUI before
continuing the original suite. The successful six-check result includes this
additional step; no application behavior was changed.

One exploratory assertion incorrectly required a `status` envelope from CLI
supersede. The command returned exit 0 and the replacement record. A separate
published-client store/supersede/get probe verified the exact corrected content;
the reviewed report documents that correction. Original failed/ineligible test
attempts remain in the VM's private artifacts and are not counted as product bugs.

Navigation retained HTTP 404 observations for `/api/plugins` and unbound workflow
channels. No JavaScript exceptions or blank pages were observed. Page rendering
is not full feature acceptance for every workflow, graph, editor or external
integration. External model subscriptions and non-Linux release installations
were not exercised. The existing one-year model certificate lifetime was not
accelerated or tested as a renewal scenario.

Reproduce the standard gates with `tests/e2e/deployment-matrix.py --topology T1`,
`T2` and `T3`, explicit digest-pinned image environment variables and an output
directory. Run `tests/e2e/postgres-luks-e2e.py` with and without `--legacy-upgrade`,
and `scripts/test-synthesis-portability.sh IMAGE` for each synthesis image.
The real native diagnostic is `unit-test-synthesis-mtls-client --managed-live`;
it uses the fixed production mTLS profile and actual published model endpoints.
Provider and model browser suites are under `scripts/validation/providers` and
`scripts/validation/setup`.

Raw logs, exact diagnostic scripts, screenshots and owned deployment state are
retained under `/opt/validation042` on VM 9434. Credentials and raw browser files
remain private. The accompanying committed JSON files contain sanitized verdicts,
not generated credentials or raw memory records. The failed upgrade fixture was
stopped, and the managed deployment was restored to its published E2B model before
final environment shutdown. VM 9434 is retained for investigation and retesting.
