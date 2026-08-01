#ifndef AIMEE_WFE_ROUNDTABLE_PROXY_H
#define AIMEE_WFE_ROUNDTABLE_PROXY_H

#include "server.h"
#include <limits.h>

#define WFE_ROUNDTABLE_DEFAULT_DEADLINE_MS 600000
#define WFE_ROUNDTABLE_TRANSPORT_GRACE_MS  30000

/* The Go coordinator gives a configured chairman its own full phase deadline.
 * The private C->Go transport must therefore cover analysis plus chairman,
 * followed by a small serialization/delivery grace. */
static inline int wfe_roundtable_transport_timeout_ms(int deadline_ms, int chairman_enabled)
{
   if (deadline_ms <= 0)
      deadline_ms = WFE_ROUNDTABLE_DEFAULT_DEADLINE_MS;
   long long phases = chairman_enabled ? 2 : 1;
   long long timeout = (long long)deadline_ms * phases + WFE_ROUNDTABLE_TRANSPORT_GRACE_MS;
   return timeout > INT_MAX ? INT_MAX : (int)timeout;
}

/* Temporary resource-plane adapter: roundtable policy and execution live in
 * Go. C only forwards the already-authorized op payload over the private UDS. */
int wfe_roundtable_proxy(server_conn_t *conn, const cJSON *request);
int handle_roundtable_review_proxy(server_ctx_t *ctx, server_conn_t *conn, cJSON *request);

#endif
