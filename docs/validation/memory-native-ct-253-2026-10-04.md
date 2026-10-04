# Native memory deployment validation on .253 — 2026-10-04

Result: passed. A fresh unprivileged CT 9203 (`aimee-native-validation-20261004`) on `192.168.1.253` built and ran the native client/server/KB and Go module fleet with `AIMEE_MEMORY_BACKEND=native`. The deployment used the same source bundle and adapted successful API probe as the [Cognee deployment](memory-cognee-ct-253-2026-10-04.md), closing the native deployment coverage gap. PostgreSQL 18.0 and pgvector 0.8.1 were built in the guest. Server and KB used separate homes/node identities and private/shared schemas with non-owner runtime roles. No shared model or compiler service was used.

| Check | Outcome |
| --- | --- |
| Private/shared memory store and search | Both shipping API paths returned their exact canonical canary. |
| Placement isolation | Each search excluded the other placement's canary. |
| Exact reads and retained retrieval | Both records remained readable and searchable after erasing an unrelated disposable subject. |
| Native erasure completion | The asynchronous shipping API reached successful terminal coverage. Repeating the same request ID completed idempotently. |
| Actual author erasure | The shipping API erased the private record's actual author; owner SQL independently confirmed zero remaining rows for that record. |
| Memory restart | The existing supervisor restarted only the native memory child. Readiness returned, the erased canonical row stayed absent, and the retained shared record remained readable. |

Deliberately stopping memory interrupted one request: it returned an error after the existing 60-second command timeout (observed 60.06 seconds). The next readiness call succeeded in 0.09 seconds. This establishes recovery and preserved erasure, not uninterrupted service during owner restart.

The disposable harness needed an explicit PostgreSQL socket directory and the packaged optional KB synthesis executable path. Both deployment setup issues were corrected before the passing probe. No runtime implementation changes were needed.

Together, the native and Cognee deployments now cover the common shipping API paths for private/shared storage, exact reads, retrieval, placement isolation, subject erasure, idempotency and memory restart. Cognee additionally passed its applicable remote-provider outage/retry, cross-scope derived cleanup and restored derived-state cleanup checks. The earlier required PostgreSQL replay and adapter race suites remain recorded in the Cognee report. These are bounded contract/lifecycle checks; production performance and inference quality were not measured. GitHub CI execution still awaits publishing the local changes.

[Machine-readable evidence](memory-native-ct-253-2026-10-04.json) records binary hashes, versions, source bundle digest, API checks, the restart interruption and cleanup. The exact probe, harness, credential-free API results and logs are retained locally in `/tmp/aimee-native-ct-report-20261004`.

Cleanup completed and was independently verified: CT/VM configuration and all `vm-9203-*` storage volumes are absent, uploaded host staging files were removed, and existing guests remained in their prior running/stopped states.
