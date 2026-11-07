/* Session briefing custody for controlled provider contexts. Human/session-start
 * exports are presentation-only. A selected native provider projection must be
 * enrolled before its text is copied into the caller's prompt. */
#ifndef AIMEE_SESSION_BRIEFING_SOURCE_H
#define AIMEE_SESSION_BRIEFING_SOURCE_H
#include "cJSON.h"
#include "request_context.h"
#include <string.h>
extern const request_context_t *request_context_get(void) __attribute__((weak));
extern int request_context_refuse_assembly(const char *) __attribute__((weak));
extern int ingress_preinject_accept_native_projection(const cJSON *) __attribute__((weak));
extern int aimee_module_tool_source_mode(const char *) __attribute__((weak));
static inline int session_briefing_provider_context(void)
{
   const request_context_t *ctx = request_context_get ? request_context_get() : NULL;
   return ctx && (ctx->model_execution || ctx->memory_receipt_required ||
                  ctx->memory_source_release[0]);
}
static inline int session_briefing_accept_source(const cJSON *response, const char *text,
                                                int provider_context)
{
   if (!provider_context || !text || !text[0])
      return 0;
   const cJSON *native = cJSON_GetObjectItemCaseSensitive(response, "native_context");
   int selected = aimee_module_tool_source_mode &&
                  aimee_module_tool_source_mode("prospective_memory") != 0;
   if (!native && !selected)
      return 0; /* Existing source-free legacy owner. */
   const char *bound = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(native, "text"));
   if (bound && !strcmp(bound, text) && ingress_preinject_accept_native_projection &&
       ingress_preinject_accept_native_projection(response) == 0)
      return 0;
   if (request_context_refuse_assembly)
      (void)request_context_refuse_assembly("unavailable");
   return -1;
}
#endif
