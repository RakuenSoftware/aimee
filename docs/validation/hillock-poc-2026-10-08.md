# Hillock HDC POC validation — 2026-10-08

The selectable `hillock` Store adapter and stateless retrieval sidecar were tested against upstream revision `1edd166ead75b85a9ab95cd6ba4faf7011ad567c`. The real-engine runner verifies the checkout revision and absence of modified tracked upstream source; this is not a fake HTTP ranker or a generated chat answer.

The final pinned-dependency run uses Python 3.12 and NumPy 2.2.6. The container/CI recipe uses Python 3.13 and the same NumPy pin. An earlier exploratory run passed with the host NumPy 2.5.3; the pinned run is the reproducible acceptance evidence. The machine's default Python 3.14 tried compiling the older NumPy pin, so acceptance used the available Python 3.12 wheel environment instead.

Executed checks:

- Real `TestHillockLiveContract`, required to execute without a skip, across two service starts: correct canonical record/unit, deterministic repeated retrieval, changed correction, deletion, no retrieval retained from a preceding request, and explicit snapshot capacity.
- Authenticated HTTP health and ranking; wrong/missing bearer, unsupported chat route, malformed identity, request limits and record-token capacity are rejected.
- Go race suites for the existing memory contract/module, Cognee, Hillock and governed egress. Hillock fixture tests reject foreign/duplicate IDs, changed/stale revisions, outages, overflow and cancellation. Configuration tests cover selecting Hillock through the existing owner; egress tests restrict the configured provider origin/routes and caller.
- Module source/package ownership, C-memory boundaries and governed egress checks pass. Proposal link, lifecycle and new audit-snapshot checks pass; unrelated historical premise-drift warnings remain report-only.

See [the fixture result](hillock-poc-2026-10-08/summary.json), [run/configuration](../../integrations/hillock/README.md) and [the full-support exposure list](../../integrations/hillock/FULL_SUPPORT_CONTRACT.md).

This establishes the bounded lexical/HDC POC, not full upstream graph/extraction/learning support, large-corpus quality, Docker-build acceptance or a live Discord deployment. Native remains the default. Persistent/semantic/adaptive support and powered answer-quality qualification remain pending.
