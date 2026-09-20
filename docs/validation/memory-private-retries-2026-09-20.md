# Private conditional-correction retry validation

The Go memory owner accepts authenticated private correction retry keys alongside
an exact expected owner/ID/revision. HTTP supersede and MCP update/supersede share
the same conditional correction path. Migration 31 persists content-free receipt
identities separately from erasable canonical history and proposal payloads.
Owner and verified principal namespace the hashed key; the digest binds request
content, expected version and effective authority. Connection identity does not
change an already committed retry namespace.

A keyed call serializes competing attempts before admission. Mutation, author,
history, invalidation and receipt insertion commit together. Review-required
model drafts are durable outcomes too; a rejected draft cannot reopen on replay.
Receipts survive parent erasure to prevent reusing a committed key. Runtime roles
cannot rewrite or erase receipts. A replay reads the current eligible result and
compares the committed version, or returns the existing visible proposal reference.
It never returns stored response content. Changed payloads conflict. Ordinary
unkeyed calls do not acquire the retry lock or query the receipt table, and proposal
replay reads references without loading draft payloads.

Private receipt UUIDs identify these transactions; they are not shared KB WORM
changeset IDs or proof that invalidation consumers caught up. Create/delete retry
keys, remaining mutation preconditions, consumer checkpoints and retention/restore
remain open MR-02 work. The wider MR-01–MR-18 acceptance program remains active.
The memory owner and module-side transport stay Go; the C bus is unchanged.

## Local evidence

The complete required PostgreSQL memory and family race suites pass (53.405 and
1.397 seconds); the final expiry regression passes in the focused race run
(1.078 seconds). Restricted-role tests cover late receipt failure with full rollback,
actor isolation, changed payload/authority conflicts, committed response loss on a
new connection, actual concurrent blocked retries, uncommitted writer disconnect,
proposal rollback, rejected and erased proposals, retired/expired result refusal,
SQL tampering refusal and authenticated host-runtime receipt forwarding. Native
MCP transport already preserves complete private owner envelopes; only its schema
description is extended to advertise the new private contract.

Fresh deployment and current-head CI evidence will be recorded after those runs
finish. Added HTTP/MCP gates exercise receipt failure, restart-safe retry, payload
conflicts, model confidence/provenance, proposal rejection and erased-key retention.
