# Atlas memory-governance follow-up

This change closes the two mechanisms that the Agent Memory Atlas did not award
to Aimee at its pinned revision: `tombstone` and `human_review`. It also addresses
the report's actionable lifecycle and scope findings instead of treating the
badges as documentation-only claims.

## Implemented contract

- A rejected typed fact creates an active tombstone keyed by the exact
  `(source, relation, target)` value. Every assertion path checks it, and a
  PostgreSQL trigger protects writers that bypass the C seam. New evidence or a
  later extraction therefore cannot resurrect the same triple.
- A rejected episodic memory remains stored with its content, reason, timestamps,
  scope and lifecycle, while normal recall excludes it. Its tombstone is keyed by
  `(key, content, scope_type, scope_value)`, so a rejection in one project does
  not poison another project.
- Restore/undo is explicit and authenticated. It deactivates rather than deletes
  the tombstone, preserving the negative decision as review history.
- Memory ownership is stored on each `memories` row. PostgreSQL RLS policies cover
  both memories and their rejection tombstones; the runtime role cannot delete or
  truncate tombstones. Legacy `memory_scopes` and `memory_workspaces` rows are
  migrated deterministically (project first, then workspace, then global).
- Recall requires `lifecycle_state='active'` independently of the two legacy
  archival feature flags. Archived, pending, rejected, fulfilled and superseded
  rows remain available to history/review APIs but cannot enter prompt context.
- Memory mutations append content-free records to the existing hash-chained WORM
  audit store in the same transaction as the state change.
- Both GUIs now have a memory review surface. The main GUI is constrained to the
  active project; the KB operator console can inspect all scopes. Both retain and
  display the full value, lifecycle, confidence, scope and rejection reason and
  expose reject/restore actions. Typed-fact approve/reject review remains in the
  existing fact console.

## Acceptance coverage

The permanent regression coverage includes:

- exact fact rejection followed by re-extraction from different evidence;
- exact episodic rejection followed by the normal merge/insert path;
- explicit restore/undo for both refusal types;
- project-A/project-B row separation with identical values;
- archived rows excluded from list and search while still directly reviewable;
- real PostgreSQL RLS, policy count, runtime privileges, trigger backstops and
  WORM evidence;
- live `aimee-server -> aimee-kb -> PostgreSQL` review/reject/reassert/restore;
- main and operator GUI rendering and mutations.

## Clean environment validation

Validation ran on fresh Debian 13 containers on `root@192.168.1.252`:

- CT 9081 (`aimee-memory-clean`): PostgreSQL 17 + pgvector, clean Git topology,
  hardened schema/RLS gate, complete C unit suite, integration suite, and live
  full-stack exploration.
- CT 9082 (`aimee-memory-migration`): PostgreSQL 17 + pgvector, upgrade from the
  pre-change `origin/testing` schema, frontend tests/builds, and Go unit suites.

The migration fixture applied the new schema twice over legacy rows. The observed
row scopes were `project:project-a`, `workspace:workspace-a`,
`workspace:workspace-a` from the older workspace table, and `global:_global` for
the untagged row, with both new RLS policies present afterward.

Final command outcomes are recorded in the implementation handoff for the commit
that contains this document; the remote containers and logs are intentionally
left intact for human inspection.
