# WFE sibling frozen-diff collision residual

- **State:** DONE — atomic sibling create claims landed in Go.
- **Archived parent:**
  [`wfe-slices-conflict-on-shared-file.md`](wfe-slices-conflict-on-shared-file.md).

## Delivered foundation

A new slice starts from the fetched remote feature tip, integrates the feature head before freezing,
and treats an actual content merge conflict as terminal rather than retrying forever.

## Delivered scope

The archived proposal's separate option 3c is now implemented: when sibling frozen diffs divergently
create the same path, the later freeze is atomically rejected with the path and both slices named
before it reaches merge. Identical create/create content and non-overlapping edits of an existing
file remain allowed.

## Acceptance

Tests cover divergent creates, identical creates, distinct-region edits, and two simultaneous
colliding freezes where exactly one succeeds. A replay of the appliance-runbook failure stops at the
second freeze with the conflicting path, and leaves no `CONFLICTING` PR or retrying merge item.

## Outcome

The Go WFE now derives exact Git blob identities for every path newly introduced by a slice's frozen
diff. DB1 publishes those claims in one write-reserved transaction, keyed by parent, path, and slice;
the transaction acquires SQLite's writer reservation before reading sibling claims, independent of
connection lock-mode configuration.
Identical sibling creations coexist, while a different blob at the same new path rejects the later
slice at `freeze` and records the path plus both work-item IDs in its terminal event. Existing-file
edits do not enter this claim set, so independently mergeable edits retain their normal path.

The claim is idempotent for refreezes and is deleted with its workflow tree. Deterministic tests drive
the full Go engine through identical creates, distinct existing-file edits, and simultaneous divergent
appliance-runbook creations; the concurrent case produces exactly one `pr` transition and one terminal
`rejected` freeze with no PR reference.
