# Typed episode source versions

Episodes can change independently of their parent memory. Typed episode selections
now observe the owner identity, episode revision and canonical parent revision in
one SQL statement. These exact string identities participate in the selection
digest and survive outer packing. An omitted episode contributes no source claim.
The rendered model text and ordinary episode read response keep their existing
shapes; no additional RPC or per-episode database query is introduced.

Shared schema 30 adds a positive episode revision. Inserts start at one, content
and provenance edits advance it, and no-op updates preserve it. Caller assignments
cannot rewind or force the counter. Existing episode IDs remain immutable.
Generated episode refreshes preserve IDs and revisions when their data is unchanged.
The C bus remains unchanged; the shared storage schema is still an external owner.

Validation:

- Full Go/PostgreSQL memory suite passes. Runtime-role race tests exercise the
  public typed command, exact IDs above 2^53, independent episode and parent
  changes, suppressed/retired/expired parents, and unchanged rendered episode
  bytes with changed parent binding.
- Projection tests cover host round trips, retained and dropped rows, changed
  revisions under an old digest, invalid channel/parent contracts, and mismatched
  episode IDs. These are consistency checks, not producer authentication.
- The migration regression preserves old episode payloads, applies the migration
  repeatedly, rejects ID changes, ignores caller-assigned counters, and checks
  independent content, session and reference-time revisions.
- [Full schema 29 → 30 → 30 upgrade](memory-episode-source-versions-2026-09-21/schema-upgrade.json)
  preserves previous correction/retirement receipts, canonical records, audit
  history and episode payloads. An additional reapplication preserves an edited
  episode revision. Audit isolation and restricted verifier permissions remain.
- All 77 lint gates, standalone export, 17 S1 tests, and documentation/link checks
  pass. Fresh application validation of schema 30 remains pending.

Five fresh authenticated KB checks were added for episode/parent binding, no-op
stability, independent provenance edits, identical rendered bytes under parent
changes, and zero-byte omission. They use a distinct authored episode key so
background indexing cannot replace the fixture.

This is observed source evidence, not a final release receipt. Transitive source
closure, restore-resistant owner incarnation, release-time authorization and
revision revalidation, unversioned aggregate/learning channels, and provider
release/dispatch binding remain open. No MR-01–MR-18 proposal is certified by
this change.
