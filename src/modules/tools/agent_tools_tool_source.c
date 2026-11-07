#include <aimee/tools/agent_tools.h>
#include <aimee/ir/aimee_ir.h>
#include "aimee_sha256.h"
#include <string.h>

/* Generic per-result custody, independent of memory policy or native grants. */
static __thread agent_tool_source_scope_t *g_tool_source_scope;
int aimee_tool_source_visible_valid(const cJSON *source, const char *visible)
{
   if (!visible || !aimee_ir_tool_source_shape_valid(source))
      return 0;
   char digest[65];
   const char *expected = cJSON_GetStringValue(
       cJSON_GetObjectItemCaseSensitive(source, "visible_sha256"));
   return aimee_sha256_hex(visible, strlen(visible), digest) == 0 && expected &&
          strcmp(digest, expected) == 0;
}
int agent_tool_source_scope_begin(agent_tool_source_scope_t *scope)
{
   if (!scope || scope->active)
      return -1;
   memset(scope, 0, sizeof(*scope));
   scope->previous = g_tool_source_scope;
   scope->active = 1;
   g_tool_source_scope = scope;
   return 0;
}
int agent_tool_source_attach(const cJSON *source, const char *visible)
{
   agent_tool_source_scope_t *scope = g_tool_source_scope;
   if (!scope)
      return -1;
   if (scope->failed || scope->tool_source || !aimee_tool_source_visible_valid(source, visible))
   {
      scope->failed = 1;
      return -1;
   }
   scope->tool_source = cJSON_Duplicate(source, 1);
   if (!scope->tool_source)
      scope->failed = 1;
   return scope->failed ? -1 : 0;
}
int agent_tool_source_check(const char *visible)
{
   agent_tool_source_scope_t *scope = g_tool_source_scope;
   if (!scope)
      return 0;
   if (scope->failed ||
       (scope->tool_source && !aimee_tool_source_visible_valid(scope->tool_source, visible)))
   {
      scope->failed = 1;
      return -1;
   }
   return 0;
}
void agent_tool_source_scope_abort(agent_tool_source_scope_t *scope)
{
   if (!scope || !scope->active)
      return;
   if (g_tool_source_scope != scope)
   {
      if (g_tool_source_scope)
         g_tool_source_scope->failed = 1;
      return;
   }
   g_tool_source_scope = scope->previous;
   cJSON_Delete(scope->tool_source);
   memset(scope, 0, sizeof(*scope));
}
int agent_tool_source_scope_finish(agent_tool_source_scope_t *scope, const char *visible,
                                   cJSON **source)
{
   if (!source)
      return -1;
   *source = NULL;
   if (!scope || !scope->active || g_tool_source_scope != scope)
      return -1;
   int rc = agent_tool_source_check(visible);
   if (rc == 0)
   {
      *source = scope->tool_source;
      scope->tool_source = NULL;
   }
   agent_tool_source_scope_abort(scope);
   return rc;
}
