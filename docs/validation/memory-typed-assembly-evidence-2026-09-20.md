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
contract/evidence tests pass.

## Remaining acceptance

This optional legacy event path records accepted assembly, not final release or
a durable preparation/dispatch state machine. Canonical source-version binding,
all-channel coverage (including the legacy plain-text facts block), mandatory
receipt persistence and crash uncertainty remain open. The legacy ordinary-memory
writer also still uses numeric JSON for source IDs; full int64 precision through
that separate storage path needs validation/repair before complete MR-06 acceptance.
Fresh authenticated writer/trace and deployment results will be recorded against
the exact application revision.
