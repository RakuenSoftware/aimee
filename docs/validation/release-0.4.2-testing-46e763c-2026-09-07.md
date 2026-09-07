# Published testing validation for 0.4.2 — NO GO until repairs ship

The published candidate at `46e763cbc68adea6f419965285f58978e45da25c` is not
ready to release. Its synthesis runtime crashes on a supported x86-64 host,
malformed memory confidence is mishandled, and disabling the browser removes
HTTP error classification. Repairs and regression tests are included in this
working branch (repair commit `67b1b3611e`). Qualification of those repairs does not repair the already
published images.

Validation used a new disposable Debian 13 VM, **9433**, on `192.168.1.253`,
with 8 vCPUs, 16 GiB RAM and an 80 GiB disk. Existing VM 9432 and production
containers were left untouched. The source came from the exact candidate Git
archive. The testing branch still named this candidate at the final branch check.
Images were pulled from GHCR; [registry digests](release-0.4.2-testing-46e763c-2026-09-07/image-digests.jsonl)
identify the published application, PostgreSQL, embedding and both synthesis variants.
The [publication run](https://github.com/RakuenSoftware/aimee/actions/runs/34089084581)
and [candidate CI](https://github.com/RakuenSoftware/aimee/actions/runs/34089084714)
finished successfully. Green CI did not cover the failures below.

## Bugs and regression coverage

### Synthesis contains instructions from the CI build host

Running the published E2B executable with `--list-devices` exited **132
(illegal instruction)** on an Intel i7-14700K. The browser reached model deployment
but the model container restarted repeatedly. `Dockerfile.llm` used llama.cpp's
native CPU build defaults.

The repair disables native tuning and builds dynamically selected CPU backends.
It advances the upstream pin from `b10218` to the published
[b10219 release](https://github.com/ggml-org/llama.cpp/releases/tag/b10219),
which contains one CLI history change. This limits upstream changes while
allowing the required newer runtime release to carry the portability repair.
`scripts/test-synthesis-portability.sh` runs the actual image executable on the
host and through QEMU with a Nehalem CPU that lacks AVX. Both `--version` and
`--list-devices` must succeed. Publication now runs this regression for each model.
The normal native mTLS regression also passed its certificate-isolation cases.

### Memory confidence silently becomes certainty or a storage error

The published HTTP store accepted a string, boolean or null confidence and stored
`1.0`. Values `-1` and `2` returned HTTP 503 claiming the memory module was
unavailable. Valid `0`, `0.25` and `1` survived round trips correctly. All 24
concurrent synthetic Unicode writes returned distinct IDs and exact contents.

The repair rejects malformed, non-finite and out-of-range values before storage,
while retaining the default for an omitted confidence. Matching personal/shared
HTTP, MCP mutation and direct KB store paths are covered by the implementation.
`test_server_memory_get.c` exercises valid endpoints, fractional and omitted
confidence and invalid values for both stores, including proving that rejection
makes no backend call. It failed on the original implementation and passes on
the repair. `memory-placement-e2e.py` now checks HTTP and MCP store/update/supersede
rejections and successful boundary values against real services.

### Disabling the browser disables HTTP error classification

The expanded confidence tests initially returned HTTP 502 with a correct
`invalid_argument` body in the browser-disabled topology. The module needed to
map that kind to HTTP 400 had been removed by `AIMEE_RUNTIME_WEB_ENABLED=0`.

The browser switch now leaves the runtime-web policy module attached. An explicit
`AIMEE_MODULE_RUNTIME_WEB=0` retains its existing meaning. The changed shell
regression failed before the repair and passes after it; the real deployment
confidence tests require HTTP 400 with the browser disabled.

### Synthesis rebuild policy

For this task, “new underlying synthesis release” is implemented as a **newer
pinned, published upstream llama.cpp build release**. A wrapper, Dockerfile base,
weight table, application or workflow edit alone cannot authorize a new synthesis
build. Such image-input edits wait for the next runtime pin advance. A release
promotion still requires the exact content-addressed image to exist; it cannot
silently promote an image missing queued source changes.

A per-image `runtime-bNNNNN` registry marker prevents publishing different images
for the same runtime release, including a reverted/reintroduced runtime pin.
The exact content tag permits an idempotent retry. The gate checks the upstream
release and refuses registry authentication or network failures instead of
interpreting them as absence. Release calls only promote existing manifests.
Tests exercise unchanged inputs, wrapper/weight-only edits, upgrades, downgrades,
missing/duplicate pins, duplicate publication, unpublished releases and outages.

## Validation results

The [local check summary](release-0.4.2-testing-46e763c-2026-09-07/local-checks.json)
records the full native suite (656 executable invocations), all 56 Python script
suites, 26 published thin-client proxy tests with the installed Codex required,
and all 77 lint checks. All passed. Candidate CI additionally passed its complete
Go, PostgreSQL, sanitizer, static analysis and frontend jobs; the
[job snapshot](release-0.4.2-testing-46e763c-2026-09-07/published-ci.tsv) distinguishes
those published-candidate checks from local repair verification.

Published images passed T1 (KB-only), T2 (separately deployed Server and KB), and
T3 (KB-free Server). Those gates include independent Vault/store identities,
personal/shared scope isolation, semantic retrieval, enrollment, restart, outage
recovery and immutable first-boot role checks. The original suites omitted the
newly discovered confidence cases.

The final repaired application reruns passed:

| Gate | Result |
| --- | --- |
| [Direct KB topology and invalid confidence](release-0.4.2-testing-46e763c-2026-09-07/final-T1/topology.json) | 12/12 |
| [KB-free memory](release-0.4.2-testing-46e763c-2026-09-07/final-T3/local-memory.json) | 49/49 |
| [Local semantic memory](release-0.4.2-testing-46e763c-2026-09-07/final-T3/semantic-memory.json) | 15/15 |
| [Connected memory](release-0.4.2-testing-46e763c-2026-09-07/final-T2/shared-memory.json) | 138/138 |
| [Immutable identities](release-0.4.2-testing-46e763c-2026-09-07/final-T2/identity.json) | 6/6 |

Both synthesis variants were rebuilt through the complete `Dockerfile.llm`,
including their baked model caches. Each passed the
[E2B](release-0.4.2-testing-46e763c-2026-09-07/synthesis-full-e2b-portability.log) and
[E4B](release-0.4.2-testing-46e763c-2026-09-07/synthesis-full-e4b-portability.log)
CPU gates. Both also passed [native discovery, inference and anonymous-client refusal](release-0.4.2-testing-46e763c-2026-09-07/full-model-inference.json)
through the managed mTLS composition, remaining healthy afterward. The anonymous
probe requires an empty response, a TLS failure and a new stunnel refusal record;
a diagnostic-text-only assertion proved sensitive to TLS connection teardown.
[Local image identities](release-0.4.2-testing-46e763c-2026-09-07/repaired-images.txt)
record the complete synthesis images and repaired application.

The repair application overlays the published image with rebuilt Server/KB
executables and the optional-module startup fix. The binaries were built with
the published Bookworm ABI. Other application modules and assets remain the
published candidate's artifacts. These local images are qualification artifacts,
not published release images.

Fresh LUKS storage and PostgreSQL 18 offline plaintext migration passed, including
preserved records, restart, crash/WAL recovery, conflicting ownership, missing or
corrupt Vault data and ciphertext preservation. This is not evidence that an
arbitrary PostgreSQL major-version upgrade is supported.

The repaired synthesis image completed [fresh browser setup](release-0.4.2-testing-46e763c-2026-09-07/browser-setup.json),
[model retirement and restoration](release-0.4.2-testing-46e763c-2026-09-07/model-lifecycle.json),
and [15-page navigation](release-0.4.2-testing-46e763c-2026-09-07/browser-navigation.json)
without JavaScript errors. The mobile Settings screenshot was inspected.
Browser credentials and screenshots remain private test artifacts.

## Reproduction

Use `tests/e2e/deployment-matrix.py --topology T1`, `T2` or `T3` with explicit
`AIMEE_APPLICATION_IMAGE`, `AIMEE_POSTGRES_IMAGE`, `AIMEE_EMBEDDER_IMAGE` and
`--output` on a disposable Linux Docker host. It creates and removes only its
randomly named projects. The new confidence checks run within the usual matrix.
Run `scripts/test-synthesis-portability.sh IMAGE` with `qemu-user-static` installed.
For inference, the registered native `unit-test-synthesis-mtls-client` supports
`--managed-live` inside an isolated managed application with its local synthesis
service running. PostgreSQL storage reproduction uses
`tests/e2e/postgres-luks-e2e.py`, with `--legacy-upgrade` for offline migration.

Raw build/test logs and the disposable images are retained under `/opt/testing042`
on VM 9433. The VM was shut down after validation to release compute resources.

## Release decision and limits

Do not promote the currently published candidate. Merge the repairs, publish and
qualify the resulting `:testing` artifacts, then satisfy the existing protected
release gates. The main-only repository lock check still fails: **the core
vendored mirror differs from its repository pin**. It requires legitimate
published repository commits and a release lock refresh; changing only digests
would fabricate provenance. No release tags or production deployments were made.

External vendor subscriptions, non-Linux installations and indefinite certificate
renewal were not exercised. The existing one-year model certificate lifetime and
PostgreSQL/Vault backup and migration requirements still apply.
