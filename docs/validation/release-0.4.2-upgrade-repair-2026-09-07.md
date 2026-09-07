# 0.4.2 upgrade repair qualification

The application upgrade blockers found in [the published d05e9d7 qualification](release-0.4.2-testing-d05e9d7-2026-09-07.md)
are repaired and the real published 0.4.1 KB upgrade passes on VM 9434, hosted by
192.168.1.253. Release approval remains pending: nine canonical source repositories do not
exist, and GitHub rejected their creation with the available personal access token.
The repair lock contains their prepared commit IDs; those nine pins are not yet published.

## Changes

- Adopt either the actual embedded KB cluster directory or the split-store `pgdata` layout;
  refuse ambiguous clusters and preserve the original read-only source.
- Discover the administrative role through the maintenance database, then rename the adopted
  `aimee_shared` database to `aimee_store`. Refuse two competing stores or an absent expected store.
- Transfer application routines by signature, including overloaded functions and the private
  Vault/WORM schemas. Preserve extension ownership, private schema access restrictions,
  routine attributes, and dedicated function owners.
- Set PostgreSQL's container-network listener explicitly while retaining TLS and SCRAM requirements.
- Add an explicit Compose Vault connection migration. Only the three SQL DSNs may replace
  existing values; ordinary startup and non-SQL credentials retain their prior behavior.
- Refresh the exact published historical memory grant that lacked the read stage. Keep both
  recorded operator restrictions and modified pre-record policies intact on Server and KB.
- Document the copied-home UID transition, rollback mounts, credentials, and one-shot command.
- Add the published 0.4.1 upgrade/rollback regression to the T1 CI deployment job.

## Runtime evidence

The final local repair images were tested on the retained release VM:

| Image | Local image ID |
| --- | --- |
| Application | `sha256:8de0f62c76631491a49a9a1a2428ed37168d51cb3e1bc9d32f4f3e8d27a41494` |
| PostgreSQL | `sha256:80ee0c169f4800ade74b766a0f295e1bc9d5c8c259ada83adcb3641f64df850e` |

PostgreSQL was built with the production Dockerfile. The application C and Go components
were built with the production Bookworm build stage, then installed with the repaired
entrypoints over the previously qualified published application digest. Browser assets,
models, and other unchanged runtime dependencies came from that published image.
These are local repair artifacts, not a newly published `:testing` qualification.

The [12-check actual upgrade](release-0.4.2-upgrade-repair-2026-09-07/repair-upgrade041-verified/results.json)
starts the pinned published 0.4.1 KB, creates global and project Unicode canaries, copies
its stopped home, and follows the shipped migration procedure. It verifies healthy startup,
credential-free application metadata, both canaries with the original authority, new writes,
persistence after container recreation, byte-for-byte preservation of the rollback cluster,
and successful original-image rollback reads.

The final LUKS gates pass [11 fresh checks](release-0.4.2-upgrade-repair-2026-09-07/repair-luks-fresh-final.json)
and [13 migration/recovery checks](release-0.4.2-upgrade-repair-2026-09-07/repair-luks-final.json).
The PostgreSQL regression passes both historical database names, routine/schema replay,
TLS network access, wrong-password rejection, runtime ownership denial, extension preservation,
private schema isolation, repeat startup, and refusal of ambiguous databases.

[Browser navigation](release-0.4.2-upgrade-repair-2026-09-07/navigation.json) passes all 15 pages
on the repaired application with the retained permanent account.
[Real local-model inference](release-0.4.2-upgrade-repair-2026-09-07/live-model-browser.json)
passes through the browser, server, and provider worker. The earlier published-image report
retains the broader exploratory, provider, E2B/E4B CPU/mTLS, frontend, and thin-client evidence.

Local checks include the Vault bootstrap unit (scoped overwrite, plaintext absence, LUKS
custody), Go PostgreSQL/storage and memory tests, six Compose bootstrap tests, actual
entrypoint grant-seeding regressions for both roles, standalone export contracts, and the
26-Go/1-C runtime-bundle checker. Generated documentation and C formatting are synchronized.

## Source publication status

The source package version advances to 0.4.2; existing tags were not replaced.
All 35 prepared repository trees were compared byte-for-byte with current exports. The
external config module retains its independent existing pin. The vendored lock checker passes.

26 repositories have published `release/0.4.2-source-pins` branches and `v0.4.2` tags; both
remote refs were checked against the lock commits. Nine still require repository creation:

- `aimee-module-aimee`
- `aimee-module-db2`
- `aimee-module-economizer`
- `aimee-module-egress`
- `aimee-module-kb`
- `aimee-module-observability`
- `aimee-module-providers`
- `aimee-module-sandbox`
- `aimee-module-server`

GitHub returned `Resource not accessible by personal access token (createRepository)`.
Their complete Git bundles and a non-forcing publication script are retained in the session
artifact `aimee-release042-pending-sources.tar.gz`. Exact commits and per-repository status
are recorded in [source-publication.json](release-0.4.2-upgrade-repair-2026-09-07/source-publication.json).
The main application release is not approved until these refs exist, repair CI passes,
and the newly published `:testing` image is qualified.
