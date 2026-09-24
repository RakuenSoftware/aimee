# MR-01 private read clock and active-context observations

Private get, list, search, validity, recall and source revalidation now use the
same request transaction setup as shared reads. All recall sections use its
captured PostgreSQL clock. Optional personal vector candidate reads use that
transaction too; a savepoint preserves lexical fallback after a vector SQL
error or timeout. Private mutation transactions keep their existing ownership.

The final serving audit reproduced an unversioned private active-context path:
nonempty task hints replace the initial versioned recall rows with lexical/dense
search results. Native projection retained their text without source references.
The owner now observes the final private payload and revision together, preserving
candidate order, current eligibility and the exact private owner. An edit after
selection refuses release. Identity, preferences and pending commitments already
observe their payload and revision in the same statement.

The [regression fails before the repair](memory-mr01-private-closeout-2026-09-24/active-context-before.txt)
and [passes after it](memory-mr01-private-closeout-2026-09-24/active-context-after.txt),
including a real PostgreSQL vector-query error with successful lexical fallback.
The private clock change separately passes the complete race suite in 287.221
seconds and exported process build in 5.806 seconds. The combined final replay
and fresh process checks remain pending. The matched Server/KB process fixture
now checks active-context source observations explicitly in both placements.
