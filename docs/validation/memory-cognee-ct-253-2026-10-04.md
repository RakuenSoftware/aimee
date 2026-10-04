# Cognee memory deployment validation on .253 — 2026-10-04

Result: passed. The current memory implementation was built from the workspace source and deployed to a new unprivileged CT 9203 (`aimee-cognee-validation-20261004`) on `192.168.1.253`. The CT and its storage were deleted after validation, and absence was verified. Existing guests remained in their prior states.

The guest ran the real native Aimee client/server/KB, Go module fleet, existing role supervisors, Vault credential helpers, PostgreSQL 18.0 with pgvector 0.8.1, and Cognee 1.6.2 on Python 3.13.5. Server and KB had separate homes/node identities and private/shared canonical namespaces. Runtime connections used non-owner roles; schema provisioning used a separate owner. Inference used deterministic guest-local fixtures. This validates protocol, storage and lifecycle behavior, not model quality or production workload capacity.

| Check | Evidence / outcome |
| --- | --- |
| Deployed private and shared store/search | Both API paths returned the exact canonical canary through real authenticated Cognee egress. Placement isolation passed. |
| Failed provider cleanup | Stopping Cognee produced a terminal erasure error with no completion coverage. The asynchronous API run was polled to its terminal result. |
| Retry with the same erasure request | Restarting Cognee allowed the existing request ID to complete, with verified absence of both nodes' derived namespaces. |
| Repeat completion | The same request completed idempotently. |
| Retained canonical records | Exact reads survived the conservative derived reset; subsequent searches rebuilt their indexes. |
| Actual author erasure | The shipping erasure API removed the private record for its actual author. Canonical SQL verification found zero matching rows, and provider cleanup completed. |
| Restored erased derived state | A historical dataset was reintroduced after canonical erasure. Restarting only the memory child through the existing supervisor removed it before serving; the canonical row remained absent. |
| Adapter CI command | Race tests for native bridge/lifecycle, Cognee adapter and egress passed inside the CT. Independent public-contract behavior is exercised by bridge/client tests. |
| Real Cognee CI runner | The checked-in runner passed with real SQLite/LanceDB/Ladybug state and local model fixtures; the live test could not skip. |
| Required PostgreSQL replay | The existing memory-owner-replay-check passed in the guest, including required owner, evaluator and process suites under the race detector. |
| Required CI aggregate | Local execution accepts memory-backends success and rejects both failure and skipped outcomes for non-documentation changes. |
| Cleanup | Proxmox removed `vm-9203-disk-0`; CT/VM configuration and all `vm-9203-*` volumes were verified absent. Uploaded host staging files were removed. |

Initial setup attempts exposed deployment prerequisites: PostgreSQL 17 lacks the required collation; packaged helper paths must exist; private/shared identities require separate homes; owner/runtime schema access and default grants must be provisioned; and the existing supervisor must restart modules whose dependencies attach later. These were corrected in the disposable harness, without adding application runtime machinery. A Cognee restart also invalidated JWTs signed with its generated process-local key; the fixture was configured with a persistent signing key. The memory owner correctly refused readiness for an invalid bearer.

The required `memory-backends` job is now in `.github/workflows/ci.yml` and included in the protected `unit-tests` aggregate. It tests the native adapter and Cognee under the race detector, installs pinned Cognee 1.6.2, and requires the real provider test to pass. GitHub execution awaits publishing the local changes; the exact test commands and live runner were validated locally and in the guest.

The complete contract, provider authoring steps, compile-checked Go example, hosting/registration, errors, scopes, cleanup semantics, Cognee operations and CI reproduction are in [the canonical memory documentation](../modules/memory.md#memory-backend-contract). Canonical documentation is included in the exported memory repository.

[Machine-readable deployment evidence](memory-cognee-ct-253-2026-10-04.json) records the source bundle digest, deployed binary hashes, versions, checks and cleanup. Detailed fixture-only logs, API results and the exact test harness are retained locally in `/tmp/aimee-cognee-ct-report-20261004`; no enrolled keys or deployment credential files were collected.

The native backend subsequently passed the common deployed API checks in a fresh CT; see [native deployment validation](memory-native-ct-253-2026-10-04.md). This closes the previously disclosed native deployment coverage gap.
