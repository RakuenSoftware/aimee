# Proposal: audit and readiness shutdown draining

- **State:** PENDING; removed from the IR/browser migration as separate work.

The migration branch included changes to audit writer handoff, bounded event
draining and readiness shutdown. These may improve delivery of pending records
during exit, but they change lifecycle behavior beyond model-message conversion.

Review the failure case and required durability guarantee independently. Reuse
the existing audit/event-bus owners, specify shutdown deadlines and cancellation,
and demonstrate delivery under concurrent writes without delaying shutdown
indefinitely. Preserve the IR module-service APIs and editor fixes separately.

The implementation is preserved at pre-removal commit
`9d257322da116facd5c9590d531dbd781d633916` and is preserved on branch
`proposal/deferred-execution-and-accounting`. Its earlier tests do not qualify a future revised patch.
