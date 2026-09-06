#ifndef AIMEE_MEMORY_BUS_CONTEXT_H
#define AIMEE_MEMORY_BUS_CONTEXT_H

#include "cJSON.h"
#include "memory_scope_query.h"

/* The KB installs its context reader before accepting requests. The server
 * does not own a DB2 request context and leaves this unregistered. */
void memory_bus_set_context_reader(void (*reader)(db2_memory_scope_context_t *));
void memory_bus_read_context(db2_memory_scope_context_t *context);

/* Carry the caller's existing thread-local context across the process edge.
 * Explicit operation fields win; the Go owner applies visibility policy. */
static inline int memory_bus_add_context(cJSON *request)
{
   if (!request)
      return -1;
   db2_memory_scope_context_t context;
   memory_bus_read_context(&context);
   if (!context.active)
      return 0;
   if ((!cJSON_HasObjectItem(request, "workspace") &&
        !cJSON_AddStringToObject(request, "workspace", context.workspace)) ||
       (!cJSON_HasObjectItem(request, "project") &&
        !cJSON_AddStringToObject(request, "project", context.project)) ||
       (!cJSON_HasObjectItem(request, "include_all") &&
        !cJSON_AddBoolToObject(request, "include_all", context.include_all)))
      return -1;
   if (context.scope_type[0] && !cJSON_HasObjectItem(request, "scope"))
   {
      cJSON *scope = cJSON_AddObjectToObject(request, "scope");
      if (!scope || !cJSON_AddStringToObject(scope, "type", context.scope_type) ||
          !cJSON_AddStringToObject(scope, "value", context.scope_value))
         return -1;
   }
   return 0;
}

#endif
