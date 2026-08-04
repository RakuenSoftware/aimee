# WFE sibling frozen-diff collision residual

- **State:** PENDING — residual scope only.
- **Archived parent:**
  [`wfe-slices-conflict-on-shared-file.md`](../done/wfe-slices-conflict-on-shared-file.md).

## Delivered foundation

A new slice starts from the fetched remote feature tip, integrates the feature head before freezing,
and treats an actual content merge conflict as terminal rather than retrying forever.

## Remaining work

Implement the archived proposal's separate option 3c: when sibling frozen diffs divergently create
the same path, atomically reject the later freeze and name the path and both slices before it reaches
merge. Identical create/create content and non-overlapping edits of an existing file must remain
allowed.

## Acceptance

Tests cover divergent creates, identical creates, distinct-region edits, and two simultaneous
colliding freezes where exactly one succeeds. A replay of the appliance-runbook failure stops at the
second freeze with the conflicting path, and leaves no `CONFLICTING` PR or retrying merge item.

