# 0.4.2 published testing qualification: a8abc2e

**Runtime qualification passed on Linux.** The published images from merged #2969 passed
fresh ordinary-storage and explicit LUKS deployments on .253, including personal/shared
memory, semantic recall, browser/model workflows, persistence, and published 0.4.1 upgrade
and rollback. This report does not approve the separate main-only release gates on #2967.

## Candidate and environment

Commit: `a8abc2e10d56ceb22a5fcdcad5ba426f0208d15e`.
[Publication](https://github.com/RakuenSoftware/aimee/actions/runs/34155974982),
[push CI](https://github.com/RakuenSoftware/aimee/actions/runs/34155974921), and
[promotion CI](https://github.com/RakuenSoftware/aimee/actions/runs/34155977718) all succeeded.
The application and PostgreSQL `:testing` image IDs matched the immutable
`:testing-a8abc2e` tags. All runtime tests used published images without replacement binaries
or local image rebuilds. The [image digests](release-0.4.2-testing-a8abc2e-2026-09-07/evidence/browser-images.json)
identify the application, PostgreSQL, embedder, E2B, and E4B artifacts.

Fresh VM **9437**, `aimee-testing-a8abc2e-042`, was created on **192.168.1.253** from Debian 13
cloud media, with eight vCPUs, 16 GiB RAM, a 120 GB disk, and address **192.168.0.220**.
Other existing VMs were not repurposed.
[Fresh-environment evidence](release-0.4.2-testing-a8abc2e-2026-09-07/evidence/fresh-environment.json)
records empty Docker images, containers, and volumes before pulling artifacts.
[Source verification](release-0.4.2-testing-a8abc2e-2026-09-07/evidence/source-verification.json)
checked all 7,054 archived files by SHA-256 with zero mismatches.

Docker was 26.1.5, Compose 2.26.1, Chromium 152, and Playwright 1.55.0. The permanent browser
validator account, SQL credentials, detailed logs, and fixture state remain private on the VM.
The browser listener is bound to guest loopback. The retained ordinary-storage project is
`aimee-e2e-server-b69532bd6c`. Its application, PostgreSQL, embedder, and E2B containers are
[healthy and match the published images](release-0.4.2-testing-a8abc2e-2026-09-07/evidence/final-runtime.json).

## Ordinary storage requires no LUKS setup

The ordinary T1, T2, T3, persistence, and upgrade suites completed before the harness loaded
loop, dm_mod, or dm_crypt or installed host cryptsetup tooling. An observation during the
published plain deployment found PostgreSQL healthy with none of those drivers present in
`/sys/module`, no added capabilities, no mapped devices, no device rules, and no privileged
container flag. Static control-device nodes existed, but the container did not receive them.
[Host/container observation](release-0.4.2-testing-a8abc2e-2026-09-07/evidence/plain-host-observation.json).
The browser-managed PostgreSQL container also
[used ordinary storage without extra privileges](release-0.4.2-testing-a8abc2e-2026-09-07/evidence/managed-storage.json).

Only after ordinary qualification completed did the harness explicitly install cryptsetup and
load the three drivers for the LUKS overlays.
[Optional host setup](release-0.4.2-testing-a8abc2e-2026-09-07/evidence/luks-host-setup.json).
Host/storage-provider encryption remains transparent to this configuration; this run did not
exercise BitLocker, FileVault, encrypted cloud storage, or Windows/macOS Docker Desktop.

## Completed results

Counts describe assertions rather than independent user journeys. The linked evidence tree
contains named verdicts with credentials and response bodies excluded.

| Area | Passed checks |
| --- | --- |
| Ordinary standalone KB, T1 | 12 topology |
| Ordinary Server plus separately enrolled KB, T2 | 17 topology, 51 personal-memory, 138 shared-memory, 6 identity |
| Ordinary KB-free Server, T3 | 4 topology, 51 personal-memory, 15 real semantic-memory |
| Explicit LUKS Server plus KB, T2 | 17 topology, 51 personal-memory, 138 shared-memory, 6 identity |
| Published 0.4.1 upgrade and rollback | 12 ordinary, 12 encrypted |
| Ordinary PostgreSQL persistence and mode boundaries | 11 |
| LUKS fresh and recovery/migration fixtures | 11 fresh, 13 migration |
| Browser setup/navigation | 6 setup, 15 pages, no JavaScript page errors |
| Providers | Normal workflows, restart persistence, negative cases, supervised outage recovery |
| HTTP/CLI exploration | 79 |
| Local models | Lifecycle changes, E2B/E4B host and Nehalem probes, live native mTLS inference, anonymous-client refusal |
| Container recreation | Published image retained, permanent login preserved, healthy services, browser inference still works |
| Published thin client | 26 proxy tests, no skips, installed Codex required |
| Frontend | 191 tests across 23 files; production build |
| Native synthesis mTLS unit | Passed |

The [runtime journal](release-0.4.2-testing-a8abc2e-2026-09-07/evidence/runtime-suite.json),
[browser journal](release-0.4.2-testing-a8abc2e-2026-09-07/evidence/browser-suite.json), and
[local checks](release-0.4.2-testing-a8abc2e-2026-09-07/regressions.json) record completed runs.
Both the bundled CLI and downloaded thin client report `testing-a8abc2e` in the
[version record](release-0.4.2-testing-a8abc2e-2026-09-07/published-versions.json).

The controlled 1.5-second exclusive lock on `user_memories` returned **HTTP 200 after
1.629 seconds** with the exact canary, without retrying the recall. The earlier 428a95d
candidate returned 502 under this experiment.
[Contention result](release-0.4.2-testing-a8abc2e-2026-09-07/evidence/recall-delay-published.json).

[Desktop](release-0.4.2-testing-a8abc2e-2026-09-07/navigation-desktop.png) and
[mobile](release-0.4.2-testing-a8abc2e-2026-09-07/navigation-mobile.png) screenshots were visually
inspected. Navigation recorded three nonfatal 404s: the optional plugin loader and two
channel-workflow lookups. `Chat.tsx` explicitly handles these as unavailable plugin support
or absent workflow state; no page errors occurred. The
[navigation record](release-0.4.2-testing-a8abc2e-2026-09-07/evidence/navigation.json)
preserves these responses and does not claim those optional endpoints returned data.

## Harness and remaining release gates

The initial browser attempt failed before Chromium started because its copied namespace
launcher lacked executable permission. That permission was corrected and browser setup and
all subsequent phases passed against the same fresh managed project. Original failed logs
are retained under the guest's private `harness-initial` directory. No application or image
change was required. The [harness](release-0.4.2-testing-a8abc2e-2026-09-07/harness) and sanitized
verdicts are supplied with this report.

[Draft promotion #2967](https://github.com/RakuenSoftware/aimee/pull/2967) still has failing
main-only [source pins](https://github.com/RakuenSoftware/aimee/actions/runs/34155977712/job/101847775014)
and [module inventory/release baseline](https://github.com/RakuenSoftware/aimee/actions/runs/34155977755/job/101847775441)
checks. Runtime qualification does not waive these gates or approve the protected release
workflow. No independent module repositories or module publications were created during
this qualification. No production source repair was needed.
