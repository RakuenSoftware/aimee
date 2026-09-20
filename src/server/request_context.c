/* request_context.c: thread-local per-request identity/transport context (#3).
 * See request_context.h. Storage only — population lives in the HTTP front-end
 * (handle_conn), which is the one place that sees the socket and headers. */
#include "request_context.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>

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

void request_context_capture_budget_header(request_context_t *ctx, const char *request)
{
   if (!ctx)
      return;
   ctx->request_budget_present = 0;
   ctx->request_budget_limits[0] = 0;
   static const char name[] = "X-Aimee-Context-Limits";
   const size_t nlen = sizeof(name) - 1;
   int prior_budget = 0;
   for (const char *line = request; line && *line;)
   {
      const char *end = strstr(line, "\r\n");
      if (!end || end == line)
         break; /* Never search the request body for a header. */
      size_t length = (size_t)(end - line);
      if (prior_budget && (*line == ' ' || *line == '\t'))
      {
         ctx->request_budget_present = -1;
         ctx->request_budget_limits[0] = 0;
         return; /* Folded limits are ambiguous, never silently truncated. */
      }
      prior_budget = 0;
      if (length > nlen && !strncasecmp(line, name, nlen) && line[nlen] == ':')
      {
         const char *value = line + nlen + 1;
         while (value < end && (*value == ' ' || *value == '\t'))
            value++;
         size_t size = (size_t)(end - value);
         if (ctx->request_budget_present || !size || size >= sizeof(ctx->request_budget_limits))
         {
            ctx->request_budget_present = -1;
            ctx->request_budget_limits[0] = 0;
            return;
         }
         prior_budget = 1;
         ctx->request_budget_present = 1;
         memcpy(ctx->request_budget_limits, value, size);
         ctx->request_budget_limits[size] = 0;
      }
      line = end + 2;
   }
}
