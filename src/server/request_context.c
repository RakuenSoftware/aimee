/* request_context.c: thread-local per-request identity/transport context (#3).
 * See request_context.h. Storage only — population lives in the HTTP front-end
 * (handle_conn), which is the one place that sees the socket and headers. */
#include "request_context.h"
#include <string.h>

static _Thread_local request_context_t g_req_ctx;
static _Thread_local int g_req_ctx_set;

void request_context_set(const request_context_t *ctx)
{
   if (!ctx)
   {
      request_context_clear();
      return;
   }
   g_req_ctx = *ctx;
   g_req_ctx_set = 1;
}

const request_context_t *request_context_get(void)
{
   return g_req_ctx_set ? &g_req_ctx : NULL;
}

void request_context_clear(void)
{
   memset(&g_req_ctx, 0, sizeof(g_req_ctx));
   g_req_ctx_set = 0;
}

const char *request_context_idempotency_key(void)
{
   return g_req_ctx_set ? g_req_ctx.idempotency_key : "";
}

const char *request_context_principal(void)
{
   return g_req_ctx_set ? g_req_ctx.principal : "";
}
