# Browser session persistence

The browser service stores each user's chat-tab ownership, resume metadata, and visible transcript.
The authenticated server record is authoritative; browser storage is only a fast cache. The full
provider conversation also stays in `aimee-server` under the stable session ID.

Stored metadata:

- session ID;
- title;
- working directory/project hint;
- created and last-active time;
- authenticated owner.
- provider-native resume ID;
- visible user/assistant transcript.

Users can list, reopen, rename, and forget only their own sessions. Sending a turn upserts the
record and transcript, so a tab survives a crash even when the frontend did not pre-register it.

The browser may cache the focused tab for fast reload, but the service is authoritative for which
tabs exist. Deleting a tab removes its browser ownership record; it does not rewrite the server's
audit history.

Thread branching and transactional turn rewind are separate contracts and must not be inferred from
tab persistence.
