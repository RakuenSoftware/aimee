# Aimee 1.1.5

A patch release in the `1.1` series. It fixes code index discovery, which could stall a session
for a long time or never finish, and lets Claude Code use the Server's Anthropic API through
`aimee launch --gateway`.

## Upgrade

- No Compose or configuration change. Take the new images as for any patch release.
- The store applies one schema migration at startup: it records progress on each code scan and
  adds an index used to reuse embeddings. Stored data is kept; nothing is rewritten.
- Update thin clients too: the background first index and the scan coordination need both sides.

## Fixes

- **Code index discovery no longer stalls the session.** In a repository the Server had not
  indexed, `aimee index investigate` (and MCP initialize) uploaded the whole tree before
  answering, which could take a long time for a large repository. The first index now runs
  as one background job per repository, and discovery answers at once that the index is being
  built.
- **Concurrent sessions no longer cancel each other's scans.** Starting a scan used to supersede
  any scan of the same project in progress, so two sessions kept cancelling each other and the
  index was never published. A scan that is making progress now keeps the project; a second
  client is told it is being indexed and leaves it alone. A scan whose client went away is taken
  over.
- **Rescans reuse stored embeddings.** Every scan embedded every file again. An unchanged file now
  reuses the embedding stored for the same path, content and embedder, so rescans no longer
  occupy the embedder (which also serves memory retrieval) for hours.
- **`aimee launch --gateway` works for enrolled clients.** The gateway sends the client's token
  with a session suffix, and the Server refused an enrolled device token in that form with 401.
- **Delegate worktrees leave no empty directories behind.**

## Logging

- A refused or failed code scan is logged on the Server with its project and phase, and the
  client gets the reason (HTTP 409) instead of an unexplained "local code index unavailable".
- A thin-client upload stops at its first failed batch instead of sending the rest.
- Gateway requests are no longer logged as refused identity tokens.

## Unchanged

- Compose files, synthesis images and embedder images are the same as 1.1.4.
- Server images remain `linux/amd64` only; thin clients are built for every platform.
