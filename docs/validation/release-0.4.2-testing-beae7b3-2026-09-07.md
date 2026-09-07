# Published testing image qualification and optional LUKS follow-up

The published `testing-beae7b3` application passed the runtime qualification on a fresh
VM on .253, including the memory contention regression from #2968. This qualifies the
Linux image exercised here; it does not approve release #2967 or establish Windows/macOS
Docker Desktop support. The follow-up change makes LUKS opt-in and is tested separately
with a locally built PostgreSQL image, as recorded below.

## Published candidate

Commit: `beae7b3f8d377aeef05530dd2fafbbbe8c56b935`.
[Testing publication](https://github.com/RakuenSoftware/aimee/actions/runs/34150771424),
[push CI](https://github.com/RakuenSoftware/aimee/actions/runs/34150771430), and
[promotion CI](https://github.com/RakuenSoftware/aimee/actions/runs/34150772774) succeeded.
The application and PostgreSQL `:testing` IDs matched their immutable
`:testing-beae7b3` tags. The [digest record](release-0.4.2-testing-beae7b3-2026-09-07/evidence/browser-images.json)
identifies all five images, including both synthesis variants.

VM **9436**, `aimee-testing-beae7b3-042`, was created on **192.168.1.253** from Debian 13
cloud media: eight vCPUs, 16 GiB RAM, 120 GB disk, address **192.168.0.244**.
[Fresh-environment evidence](release-0.4.2-testing-beae7b3-2026-09-07/evidence/fresh-environment.json)
records empty Docker images, containers, and volumes before deployment.
[Source verification](release-0.4.2-testing-beae7b3-2026-09-07/evidence/source-verification.json)
checked 6,971 archived candidate files with zero SHA-256 mismatches. The published-image
qualification used the published binaries without replacement or source overlays.
Other existing VMs were not repurposed.

Docker was 26.1.5, Compose 2.26.1, Chromium 152, and Playwright 1.55.0. The retained
managed project is `aimee-e2e-server-157b0cefab`, with its browser bound to guest loopback.
Its permanent validator credentials and detailed logs remain private on the VM.
[Final health](release-0.4.2-testing-beae7b3-2026-09-07/evidence/final-runtime.json)
confirms healthy application, PostgreSQL, embedder, and E2B containers using the published images.

## Published-image results

Counts are assertions, not independent user journeys. JSON files contain named verdicts;
credential-bearing fields and response bodies are excluded.

| Area | Passed assertions |
| --- | --- |
| Standalone KB, T1 | 12 topology |
| Server plus separately enrolled KB, T2 | 17 topology, 51 personal-memory, 138 shared-memory, 6 identity |
| KB-free Server, T3 | 4 topology, 51 personal-memory, 15 real semantic-memory |
| Published 0.4.1 upgrade and rollback | 12 |
| LUKS fresh and recovery/migration fixtures | 11 fresh, 13 migration |
| Browser setup and navigation | 6 setup, 15 pages, no JavaScript page errors |
| Provider workflows | 9 normal, 4 after restart, 5 negative cases, outage recovery |
| HTTP/CLI exploration | 79 |
| Models | 2 lifecycle, 8 inference/mTLS/host and Nehalem probes; browser inference before and after recreation |
| Thin client artifact | 26, no skips, using installed Codex 0.153.4 |
| Frontend | 191 tests in 23 files, production build |
| Native synthesis mTLS unit | Passed |

The [runtime journal](release-0.4.2-testing-beae7b3-2026-09-07/evidence/runtime-suite.json),
[browser journal](release-0.4.2-testing-beae7b3-2026-09-07/evidence/browser-suite.json), and
[local regression results](release-0.4.2-testing-beae7b3-2026-09-07/regressions.json)
record the completed runs. The downloaded thin client and bundled CLI both identify
`testing-beae7b3` in the [version record](release-0.4.2-testing-beae7b3-2026-09-07/published-versions.json).

A controlled 1.5-second exclusive lock on `user_memories` now returns **HTTP 200 after
1.693 seconds**, including the exact canary, without retrying the request. The prior
published candidate returned 502 after 0.579 seconds under the same experiment.
[Contention evidence](release-0.4.2-testing-beae7b3-2026-09-07/evidence/recall-delay-published.json).

[Desktop](release-0.4.2-testing-beae7b3-2026-09-07/navigation-desktop.png) and
[mobile](release-0.4.2-testing-beae7b3-2026-09-07/navigation-mobile.png) screenshots were
visually inspected. Navigation recorded nonfatal 404 responses for `/api/plugins` and two
session workflow-channel lookups. These remain in the
[navigation evidence](release-0.4.2-testing-beae7b3-2026-09-07/evidence/navigation.json);
passing page rendering does not establish those endpoints as working.

## Fresh-host test setup corrections

The initial guest did not have loop, dm_mod, or dm_crypt loaded. Host discovery failed
before candidate startup. Those drivers were loaded and configured for boot, and the
runtime and browser suites were rerun. Later, the LUKS inspection fixtures found the host
`cryptsetup` executable missing, after the encrypted database had started. Installing
`cryptsetup-bin` allowed those two fixtures to finish successfully on rerun. Original
failed logs are retained privately under `host-setup-initial` and `host-setup-cryptsetup`;
these corrections did not change the published application or PostgreSQL image.

## Opt-in LUKS implementation

The follow-up defaults the PostgreSQL image to `plain` storage on its ordinary named
volume. Default Server, KB, and managed Compose definitions have no storage device
mappings, extra PostgreSQL capabilities, or Vault unlock socket. The launcher uses the
selected Docker context without inspecting the invoking machine's kernel or devices.

`compose.luks.yaml` and `compose.kb.luks.yaml` explicitly select `luks`, attach the
owning application's unlock socket, and add the required Linux device permissions.
The operator supplies host support only for that option. TLS, scoped SQL roles, and
Vault-backed application credentials remain in both modes. Plain mode does not provide
database encryption at rest.

The historical PostgreSQL volume identity is retained. Both modes reject an existing
store of the other type, preventing an omitted overlay from silently replacing an
existing encrypted database. Both use the verified offline PostgreSQL 18 migration and
its durable completion marker; restarting does not replay migration over later writes.

Validation uses a full build of `Dockerfile.postgres`, tagged only on the disposable VM
as `aimee-postgres:optin-local`, together with the unchanged published application and
model images. No container image or independent module was published by this validation.
The original published managed stack remains intact. CI runs encrypted coverage in a
separate job while preserving the three existing plain topology check names.

### Opt-in change results

All runs passed with the [locally built image](release-0.4.2-testing-beae7b3-2026-09-07/optin/image.json).
The 13 production build inputs matched the working tree by SHA-256 in the
[source verification](release-0.4.2-testing-beae7b3-2026-09-07/optin/source-verification.json).

| Mode | Completed runtime checks |
| --- | --- |
| Plain | T1, T2, T3; published 0.4.1 upgrade and rollback |
| LUKS | T2; published 0.4.1 upgrade and rollback; 11 fresh and 13 recovery/migration assertions |
| Storage boundary | 11 checks: no extra host capabilities/devices, TLS enforcement, restricted runtime role, restart/recreation persistence, refused mode switches, preserved original store |
| Compose | All six Server/KB/managed combinations, with and without LUKS |

[Runtime journal](release-0.4.2-testing-beae7b3-2026-09-07/optin/runtime-suite.json),
[encrypted upgrade](release-0.4.2-testing-beae7b3-2026-09-07/optin/luks-upgrade041/results.json),
[storage boundaries](release-0.4.2-testing-beae7b3-2026-09-07/optin/plain-storage.json), and
[Compose models](release-0.4.2-testing-beae7b3-2026-09-07/optin/compositions.json).

[Local checks](release-0.4.2-testing-beae7b3-2026-09-07/optin/local-checks.json) include
Go tests, race detection and vet, packaging mutation checks, seven bootstrap/launcher tests,
and actionlint. All 77 repository lint checks ran; the only failure was the new file missing
from the existing PostgreSQL descriptor. After registration, schema-sync, module inventory,
and package ownership checks passed. The published evidence passed secret scanning.

The [harness sources](release-0.4.2-testing-beae7b3-2026-09-07/harness) retain the run
commands. Published qualification uses `/opt/validation042/source`; local opt-in testing
uses `/opt/validation042/source-optin`. Neither harness publishes artifacts.

## Release limitations

The [draft 0.4.2 promotion](https://github.com/RakuenSoftware/aimee/pull/2967) still has
main-only source-pin and module-inventory/release-baseline gates to resolve. This work
does not waive them, publish independent modules, merge the promotion, or approve its
protected release environment. The opt-in change requires a subsequently published
image before it can be qualified as the release artifact.

No Windows or macOS Docker Desktop host was available in this run. Removing the LUKS
host dependency establishes the intended default configuration; Linux VM results alone
do not prove every Docker backend supported.
