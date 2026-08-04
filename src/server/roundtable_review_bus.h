#ifndef AIMEE_ROUNDTABLE_REVIEW_BUS_H
#define AIMEE_ROUNDTABLE_REVIEW_BUS_H

#include "server.h"
#include <limits.h>

#define ROUNDTABLE_REVIEW_DEFAULT_DEADLINE_MS 600000
#define ROUNDTABLE_REVIEW_GRACE_MS            30000

/* A configured chairman gets its own full phase deadline, so the call has to
 * cover analysis plus chairman, followed by a small serialization grace. Sharing
 * one phase deadline starved the chairman to nothing whenever the seats ran
 * long, and it failed on the request that launches its own turn. */
static inline int roundtable_review_deadline_ms(int deadline_ms, int chairman_enabled)
{
   if (deadline_ms <= 0)
      deadline_ms = ROUNDTABLE_REVIEW_DEFAULT_DEADLINE_MS;
   long long phases = chairman_enabled ? 2 : 1;
   long long timeout = (long long)deadline_ms * phases + ROUNDTABLE_REVIEW_GRACE_MS;
   return timeout > INT_MAX ? INT_MAX : (int)timeout;
}

int handle_roundtable_review(server_ctx_t *ctx, server_conn_t *conn, cJSON *request);

#endif
