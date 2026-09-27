/* Connections made available to authenticated IR plans. Selection, ordering,
 * rendering and context policy belong to the module producing the plan. */
#include "ir_host_bindings.h"
#include "aimee_session_guidance.h"
#include "ingress_preinject.h"
#include "module_commands.h"
#include "log.h"
#include "request_context.h"
#include <aimee/core/turn_integrity.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern int learning_evidence_write_retrieval_event(const char *query_fingerprint, const char *role,
                                                   const int64_t *surfaced_ids, int n_surfaced,
                                                   char *id_out, int id_out_len)
    __attribute__((weak));
static const char *field(const cJSON *args, const char *name)
{
   return cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(args, name));
}
static cJSON *context_call(const cJSON *args, void *context)
{
   (void)context;
   const char *query = field(args, "query");
   if (!query)
      return NULL;
   char *text = ingress_preinject_build(query, 0);
   cJSON *result = text ? cJSON_CreateString(text) : NULL;
   free(text);
   return result;
}
static cJSON *audit_call(const cJSON *args, void *context)
{
   (void)context;
   const char *fingerprint = field(args, "query_fingerprint"), *role = field(args, "role");
   if (!fingerprint || !role || !learning_evidence_write_retrieval_event)
      return NULL;
   int rc = learning_evidence_write_retrieval_event(fingerprint, role, NULL, 0, NULL, 0);
   return cJSON_CreateBool(rc == 0);
}
static cJSON *epoch_call(const cJSON *args, void *context)
{
   (void)context;
   const char *domain = field(args, "domain"), *scope = field(args, "scope");
   if (!domain || !scope)
      return NULL;
   char epoch[32];
   snprintf(epoch, sizeof(epoch), "%" PRIu64, ti_knowledge_epoch_current(domain, scope));
   return cJSON_CreateString(epoch);
}
static cJSON *log_call(const cJSON *args, void *context)
{
   (void)context;
   const char *module = field(args, "module"), *message = field(args, "message");
   if (!module || !message)
      return NULL;
   LOG_INFO(module, "%s", message);
   return cJSON_CreateBool(1);
}
const aimee_ir_plan_binding_t server_ir_plan_bindings[] = {{"context", context_call},
                                                           {"audit", audit_call},
                                                           {"epoch", epoch_call},
                                                           {"log", log_call},
                                                           {NULL, NULL}};
const aimee_ir_plan_resource_t server_ir_plan_resources[] = {{"guidance", AIMEE_GUIDANCE_BLOCK},
                                                             {NULL, NULL}};
/* Lean plan consumers may have no HTTP request context. Production HTTP hosts
 * supply the request-scoped sink, including copied asynchronous workers. */
extern int request_context_refuse_assembly(const char *kind) __attribute__((weak));
void server_ir_plan_refuse(const char *kind, void *context)
{
   (void)context;
   if (request_context_refuse_assembly)
      (void)request_context_refuse_assembly(kind);
}
char *server_ir_plan_text(const char *method, const char *operation, const char *phase,
                          const char *query)
{
   const aimee_ir_module_plan_t config = {.method = method,
                                          .operation = operation,
                                          .phase = phase,
                                          .provided_query = query,
                                          .bindings = server_ir_plan_bindings,
                                          .refuse = server_ir_plan_refuse,
                                          .resources = server_ir_plan_resources};
   return aimee_ir_module_plan_text(&config);
}
int server_ir_plan_enabled(const char *method, const char *operation, const char *value)
{
   cJSON *args = cJSON_CreateObject(), *reply = NULL;
   if (!args || !cJSON_AddStringToObject(args, "operation", operation) ||
       !cJSON_AddStringToObject(args, "value", value ? value : ""))
   {
      cJSON_Delete(args);
      return 1;
   }
   int rc = aimee_module_commands_dispatch_internal_timeout(method, args, 500, &reply);
   cJSON_Delete(args);
   const char *status = field(reply, "status");
   int enabled = !(rc > 0 && status && strcmp(status, "ok") == 0 &&
                   cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(reply, "enabled")));
   cJSON_Delete(reply);
   return enabled;
}
