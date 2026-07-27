# Anchored editing

Anchored editing addresses a span by content-derived line anchors instead of trusting line numbers
from an earlier read. It makes a stale edit fail before it changes the wrong text.

## Read

An anchored read returns each line with a short hash tied to its current content. The caller keeps
the anchors around the intended edit.

## Edit

An edit names the expected anchor range and replacement. The server:

1. resolves the file inside the assigned workspace;
2. rereads current bytes;
3. verifies the anchors and adjacency;
4. applies the replacement atomically;
5. returns new anchors or a conflict.

If another writer changed the span, the edit fails. The caller rereads and decides again. It must not
search for a vaguely similar block and write there.

## Properties

- line insertions elsewhere do not invalidate the target;
- edits to the target do invalidate it;
- duplicate text needs surrounding anchors to disambiguate;
- newline style and final newline are preserved;
- the path and write authority are checked before content matching;
- an empty or over-broad match is refused.

Anchors are conflict detectors, not signatures or authorization.

## Adjacent tools

Exact symbol reads can return anchored spans. Search and code-graph results locate candidates; an
anchored read fixes the exact revision before write. Blast-radius checks remain separate.

Lean web search applies the same economy to network evidence: fetch the bounded text needed for a
claim, preserve the URL and retrieval metadata, and avoid copying a whole page into model context.

Network fetches enforce scheme, DNS/IP, redirect, body, time, and content-type policy to prevent
SSRF and unbounded downloads.

## Migration

Legacy line-number edits remain compatibility inputs only where the route says so. New agent and
tool contracts should use anchored spans. A provider does not need to know the hash algorithm; it
only needs to return the anchors it was given.

## Verify

Test concurrent insertion, target mutation, duplicate blocks, CRLF/LF, Unicode, symlink escape,
partial writes, and file replacement between read and commit. Measure incorrect-edit rate, not only
tool success rate.
