# Typed assembly evidence after host integrity acceptance

Go ingress assembly now returns `retained_typed_refs` for its actual retained
rows. The host forwards these opaque references to the existing retrieval-event
writer only after the envelope passes integrity checks and its output allocation
succeeds. The existing evidence-emission setting still applies. Typed-only
assemblies can emit; omitted rows, rejected/empty envelopes and failed assembly
cannot. Ordinary memory emission happens first because that legacy turn writer
is first-wins; the typed references then merge into the same turn event.

Each reference has type `memory_projection_item` and opaque identity
`typed:v1:<selection_digest>:<channel>:<stable_id>`. The final selection digest
binds the rendered typed projection and retained IDs. Different accepted
projections of the same source remain distinguishable, while a repeated identical
projection deduplicates. These are projection-occurrence identities, not canonical
source revisions. No `v` value is invented. They do not record provider dispatch,
acknowledgement or successful application, and do not feed the legacy ordinary-
memory outcome bridge.

Selection, identity construction and packing remain in Go. The C host only
forwards owner-selected strings; the C bus remains C. The existing external
storage/audit writer is reused, not migrated or presented as the final governed
receipt pipeline.

## Bounded trace reader repair

Inspection found the external storage adapter used snprintf to truncate event
IDs and payloads into caller buffers while returning success. The HTTP trace
reader has an 8 KiB buffer, so larger events could be reported as successful
partial raw JSON. The adapter now refuses insufficient buffers, leaves both
outputs empty and returns the existing error state. The HTTP reader consequently
reports evidence unavailable rather than fabricating a complete trace. Exact-fit
reads and ID-only lookups remain supported; stored events are unchanged.

A regression failed against the previous implementation on an undersized payload
buffer, then passed for exact fits, payload overflow and identity overflow. The
full native demotion writer/merge suite passes with the repository's Go config
default fixture. Native ingress and IR module-plan tests use the real Go memory
subprocess and verify partial selection, typed-only emission, final selection
identity, writer ordering, configuration opt-out and integrity/assembly failures.
The required PostgreSQL-backed race suites pass: memory 52.032 seconds, families
1.404 seconds. Ownership and C boundary checks and all 17 semantic-context
contract/evidence tests pass. The native fidelity, fidelity-check and evidence-replay
consumer suites also pass against the repaired reader.

## Remaining acceptance

This optional legacy event path records accepted assembly, not final release or
a durable preparation/dispatch state machine. Canonical source-version binding,
all-channel coverage (including the legacy plain-text facts block), mandatory
receipt persistence and crash uncertainty remain open. The ordinary-memory ID precision gap identified in this run is repaired in the
[exact evidence identity follow-up](memory-exact-evidence-ids-2026-09-20.md),
including the matching attribution and provenance consumers.

## Fresh deployment results

Application/harness `440144e437` passes **835/835 checks** on `.253` CT 9498:
**545/545** in enrolled T2 and **290/290** in standalone T3. Eleven new
checks exercise authenticated typed-reference writing and tracing, duplicate
merges, distinct selection identities, empty-event avoidance and oversized trace
refusal. A subsequent merge retry still reaches the unchanged large stored event.
Existing private/shared revision, review, erasure, restart, rollback, outage,
semantic, identity, exploratory and 160 provider-boundary checks pass.

[Named T2 verdicts](memory-shared-reliability-2026-09-20/fresh-t2-440144e437.json),
[named T3 verdicts](memory-shared-reliability-2026-09-20/fresh-t3-440144e437.json),
[provider accounting](memory-shared-reliability-2026-09-20/provider-accounting-440144e437.json)
and [all nine image identities](memory-shared-reliability-2026-09-20/image-identities-440144e437.json)
contain no prompt bodies or credentials. Application image:
`sha256:6352d4c6f195d467641cc8b2fc4be823d78a715f11c754150daf8271c693cde3`.
The PostgreSQL and embedder images match the preceding validated run. Raw results
remain at `/opt/aimee-memory-proposals-evidence/t2-440144e437` and
`t3-440144e437`. Tested containers were stopped; volumes and receipts remain.
The subsequent evidence commit changes documentation only.

The fresh gate exercises real authenticated persistence and bounded trace reads.
Host integrity-gated automatic emission is exercised separately by the native
C-host/Go-process tests. The provider captures still use caller-supplied recall
projections and do not certify automatic workspace ingress or durable dispatch.
No new whole-request P95 or token-cost improvement is claimed.
