# MR-01 structured recall source versions — 2026-09-24

Directives and reminders now carry owner-bound record revisions into native
recall projections and source revalidation. Directive observations include the
complete referenced memory-parent versions. Persistent revisions detect edits
and edits subsequently restored, while ignoring surfacing counters. A normal
one-shot acknowledgement preserves an explicitly retained reminder observation;
new selection still excludes the consumed reminder. Other state changes advance
the revision.

The additive shipping DDL is tested against legacy tables and repeated
application. It preserves existing revisions and ignores generated full-text
columns when comparing semantic content. No draft schema was applied to CT100.

The [full memory race suite and C export build](memory-mr01-structured-sources-2026-09-24/race-export.txt)
passed (289.534 seconds and 5.636 seconds). Restricted-role tests cover parent
edits, revocation, scope changes, directive/reminder edits and restoration,
expiry, state transitions, counters, and one-shot acknowledgement. The
[first full run](memory-mr01-structured-sources-2026-09-24/initial-full-failure.txt)
exposed two test-fixture issues, corrected before the passing run: a missing
collection-owner row and overlapping migration extraction markers.

The HTTP fixture adds 16 structured-source checks; candidate process validation
is pending. These versions do not close the post-check mutation race. MR-01 A5
and overall closeout remain open.
