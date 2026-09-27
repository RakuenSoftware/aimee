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
1.397 seconds); the final expiry and approved-proposal regressions pass in the focused race run
(1.089 seconds). Restricted-role tests cover late receipt failure with full rollback,
actor isolation, changed payload/authority conflicts, committed response loss on a
new connection, actual concurrent blocked retries, uncommitted writer disconnect,
proposal rollback, approved/rejected and erased proposals, retired/expired result refusal,
SQL tampering refusal and authenticated host-runtime receipt forwarding. Native
MCP transport already preserves complete private owner envelopes; only its schema
description is extended to advertise the new private contract.

## Fresh deployment evidence

Application and harness `88ebfc8460` passed **638/638** checks in new owned
T2 and T3 deployments on `.253`, CT 9498:

- T2: **429/429** (168 private, 208 shared, 29 shared review, six identity
  and 18 topology checks).
- T3: **209/209** (168 private, 15 real-model semantic, 21 exploratory
  and five topology checks).

The [T2 receipt](memory-shared-reliability-2026-09-20/fresh-t2-88ebfc8460.json)
and [T3 receipt](memory-shared-reliability-2026-09-20/fresh-t3-88ebfc8460.json)
contain only verdict names and booleans. Each private run adds 33 checks covering
HTTP receipt failure and atomic rollback, restart-safe retries, changed payloads,
MCP model confidence/provenance, proposal rejection and erased-key retention.
The shared MCP fixture also verifies that a shared owner/version cannot be used
for a private keyed correction. Existing shared isolation, rollback, outage and
restart tests, real-model semantic tests and exploratory concurrency/int64/Go-owner
failure-recovery tests continue to pass.

All nine containers were independently checked against their image IDs:

- Application: `sha256:425295f768166cca52012961490fdce4d052d1a02f03bfffac9f3ffd1ebeea38`.
- PostgreSQL: `sha256:b6209cde68c9a7a65c562b8a4ca45682f138b4a2de5b5dbcfe7ca04ec48e962f`.
- Embedder: `sha256:f1286af7de10cf058a9bec14c45326d64de73e3878db19c732db86cda1f9d979`.

Raw evidence is retained in `/opt/aimee-memory-proposals-evidence/t2-88ebfc8460`
and `t3-88ebfc8460`. Containers are stopped; volumes and evidence remain. The
follow-up adds the approved-proposal retry regression and this evidence; it does
not change application code. No new whole-request performance claim is made.

Full CI passed on predecessor `2268a64429`
([run 35528253764](https://github.com/RakuenSoftware/aimee/actions/runs/35528253764)).
CI on this implementation remains pending and is not implied by that earlier run.
