/* browser_session_owner.h: admit an attested browser actor as a session's owner.
 *
 * Header-only because both callers (the attach route and chat-turn admission)
 * link into many test binaries; a new translation unit would have to be added to
 * every one of them for a dozen lines of policy. */
#ifndef AIMEE_SERVER_BROWSER_SESSION_OWNER_H
#define AIMEE_SERVER_BROWSER_SESSION_OWNER_H

#include <string.h>

#include "db1_client/server_sessions.h"

enum
{
   BROWSER_SESSION_OWNED = 0,        /* `actor` owns the session (possibly just now) */
   BROWSER_SESSION_UNAVAILABLE = -1, /* the durable row could not be read or written */
   BROWSER_SESSION_FOREIGN = -2,     /* another principal owns it */
};

/* Make `actor` the owner of browser session `session` when it is free to be:
 *
 * - no row yet: create one owned by `actor`;
 * - a row with an EMPTY principal: a tab created before sessions carried an
 *   owner. The webchat create claims it (only an ownerless chat/webchat row is
 *   ever claimed; see serverSessionCreate). Without this every pre-upgrade tab
 *   was refused at attach with "browser session owner mismatch", which left the
 *   browser's agent picker disabled and its turns unstartable;
 * - a row owned by `actor`: nothing to do.
 *
 * Creation never changes an existing owner: concurrent creators and lost
 * acknowledgements are resolved only by re-reading the durable row. */
static inline int browser_session_admit_owner(const char *session, const char *actor,
                                              db1_server_session_owner_t *row)
{
   if (!session || !session[0] || !actor || !actor[0] || !row)
      return BROWSER_SESSION_UNAVAILABLE;
   if (db1_server_session_owner_get(session, row) == 0 && row->principal[0])
      return strcmp(row->principal, actor) ? BROWSER_SESSION_FOREIGN : BROWSER_SESSION_OWNED;
   (void)db1_server_session_create(session, "webchat", actor);
   if (db1_server_session_owner_get(session, row) != 0)
      return BROWSER_SESSION_UNAVAILABLE;
   return strcmp(row->principal, actor) ? BROWSER_SESSION_FOREIGN : BROWSER_SESSION_OWNED;
}

#endif
