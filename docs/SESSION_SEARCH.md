# Canonical session search

`session_search` reads only the authenticated actor's durable canonical session
log. It never searches legacy provider transcripts or an owner-less session.
The current view is the default and uses the same canonical history reducer as
execution, including imported history checkpoints.

Use `view: "observations"` to search original message observations that a later
compaction removed from current history. This is a separate named view, not a
text-deduplicated merge of checkpoints and observations. Identical messages stay
distinct when their original message identities differ. Checkpoint-only history
belongs to the current view; it does not acquire invented original message IDs.

Discovery retains `sessions`, `match`, `window` and bookends. Browse retains
`preview` and `message_count`. Scroll retains `found`, `window` and
`around_message_id`. Each session result also identifies its `view` and
`source_revision`. Include the same `session_id`, `view` and `source_revision`
when scrolling to reject a changed source. A revision never grants access to a
session or another actor. Legacy `agent_name` and `provider` fields stay present
but are empty when the canonical source has no authoritative registration data.

Snippets include text and plain-string tool results. Thinking, tool arguments,
structured opaque tool results, hidden context and confidential/restricted
context do not become searchable text. Long display text is clipped to 4,096
Unicode characters and marked `text_truncated`; original canonical storage is
unchanged. `include_sources` explicitly opts into delegate/internal/tool/MCP
sessions as before. Bounded discovery reports `has_more` when its candidate
budget is exhausted and provides `next_offset`; pass that value as `offset`
with the same query, view and filters to continue. Current history that requires reconciliation refuses rather
than guessing a conversation; the observations view remains explicitly selectable.

The 2 MiB encoded response limit also bounds pages when a valid `limit` and
`window` would otherwise produce a larger result. Each session window and its
bookends stay together; `next_offset` leaves the next unreturned candidate for
the following page. JSON escapes count toward this bound. If a single session's
display metadata/window alone exceeds the limit, search reports an explicit size
error rather than silently dropping the source; a smaller window may help.
`offset` is only for multi-session browsing/discovery. A nonzero offset with an
explicit `session_id` and no query is invalid, including revision-bound scrolls.
Visible blocks within a message are searched in their original order, separated
by newlines, exactly as they appear in the text snippet.

Multi-session current-view searches skip unfinished executions so an active turn
cannot hide completed matches. Selecting that session explicitly reports that
current history is unfinished and offers the observations view. Malformed
canonical history still fails strictly; this policy does not reconcile or resume
an interrupted execution.
