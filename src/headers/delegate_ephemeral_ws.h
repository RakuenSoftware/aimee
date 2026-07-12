#ifndef DELEGATE_EPHEMERAL_WS_H
#define DELEGATE_EPHEMERAL_WS_H

#include <stddef.h>

/* Result a shell tool returns when a DETACHED (client-served) workspace's reverse
 * channel is unusable — the serving client is not connected (e.g. a background/
 * durable delegate whose dispatching client disconnected). Shared by both shell
 * executors so the wording never drifts. ASCII only (no em-dash) for portability
 * across log pipelines / non-UTF-8 environments. */
#define DELEGATE_DETACHED_CHANNEL_DOWN_JSON                                                        \
   "{\"stdout\":\"\",\"stderr\":\"detached workspace reverse-channel unavailable: the serving "    \
   "client is not connected -- a background/durable delegate cannot run shell tools against a "    \
   "client-served (detached) workspace\",\"exit_code\":-1}"

/* Create a server-side ephemeral workspace for a background delegate whose
 * detached (client-served) workspace has no live client. Validates deleg_id
 * (non-empty, <=128 chars, only [A-Za-z0-9._-], no path separators or ".."),
 * builds <aimee_home>/delegate-ws/<deleg_id>, creates it mode 0700, and drops an
 * AIMEE_WORKSPACE_NOTE.txt explaining the client repo is NOT present (so read-only
 * tools like grep/git surface the note rather than misleading empty results).
 *
 * On success returns 0 and writes the absolute path into out (out_cap bytes). On
 * ANY failure returns -1 and leaves out empty — the caller must fail closed and
 * NOT proceed as if a workspace exists. */
int delegate_ephemeral_ws_create(const char *deleg_id, char *out, size_t out_cap);

/* Best-effort recursive removal of a path previously returned by
 * delegate_ephemeral_ws_create. Refuses any path not under <aimee_home>/
 * delegate-ws/ (or containing "..") so it can never remove anything else. */
void delegate_ephemeral_ws_remove(const char *path);

#endif /* DELEGATE_EPHEMERAL_WS_H */
