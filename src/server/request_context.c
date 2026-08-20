/* request_context.c: thread-local per-request identity/transport context (#3).
 * See request_context.h. Storage only — population lives in the HTTP front-end
 * (handle_conn), which is the one place that sees the socket and headers. */
#include "request_context.h"
#include <stdio.h>
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

const char *request_context_caller_subject(void)
{
   return g_req_ctx_set ? g_req_ctx.caller_subject : "";
}

const char *request_context_caller_authorization(void)
{
   return g_req_ctx_set ? g_req_ctx.caller_authorization : "";
}

void request_context_override_principal(const char *principal)
{
   if (!g_req_ctx_set || !principal || !principal[0])
      return;
   snprintf(g_req_ctx.principal, sizeof(g_req_ctx.principal), "%s", principal);
   g_req_ctx.trusted = 1;
}

void request_context_override_caller_subject(const char *subject)
{
   if (!g_req_ctx_set || !subject || !subject[0])
      return;
   memset(g_req_ctx.caller_authorization, 0, sizeof(g_req_ctx.caller_authorization));
   snprintf(g_req_ctx.caller_subject, sizeof(g_req_ctx.caller_subject), "%s", subject);
}

void request_context_override_caller_authorization(const char *jwt)
{
   if (!g_req_ctx_set || !jwt || !jwt[0] || strlen(jwt) >= sizeof(g_req_ctx.caller_authorization))
      return;
   memset(g_req_ctx.caller_subject, 0, sizeof(g_req_ctx.caller_subject));
   snprintf(g_req_ctx.caller_authorization, sizeof(g_req_ctx.caller_authorization), "%s", jwt);
}

void request_context_note_aimee_session(int tool_calls, int redundant_tool_calls,
                                        const char *intervention, const char *tool_transport)
{
   if (!g_req_ctx_set)
      return;
   g_req_ctx.aimee_tool_calls = tool_calls;
   g_req_ctx.aimee_redundant_tool_calls = redundant_tool_calls;
   snprintf(g_req_ctx.aimee_intervention, sizeof(g_req_ctx.aimee_intervention), "%s",
            intervention ? intervention : "");
   snprintf(g_req_ctx.aimee_tool_transport, sizeof(g_req_ctx.aimee_tool_transport), "%s",
            tool_transport ? tool_transport : "none");
}
