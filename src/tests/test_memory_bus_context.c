#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "modules/memory/memory_bus_context.h"

static db2_memory_scope_context_t current;

void memory_bus_read_context(db2_memory_scope_context_t *context)
{
   *context = current;
}

int main(void)
{
   cJSON *request = cJSON_CreateObject();
   assert(memory_bus_add_context(request) == 0);
   assert(!cJSON_HasObjectItem(request, "scope"));
   assert(!cJSON_HasObjectItem(request, "project"));
   cJSON_Delete(request);

   current.active = 1;
   strcpy(current.workspace, "workspace-a");
   strcpy(current.project, "project-a");
   strcpy(current.scope_type, "project");
   strcpy(current.scope_value, "project-a");
   request = cJSON_CreateObject();
   assert(memory_bus_add_context(request) == 0);
   assert(strcmp(cJSON_GetObjectItem(request, "project")->valuestring, "project-a") == 0);
   assert(strcmp(cJSON_GetObjectItem(request, "workspace")->valuestring, "workspace-a") == 0);
   assert(cJSON_IsFalse(cJSON_GetObjectItem(request, "include_all")));
   cJSON *scope = cJSON_GetObjectItem(request, "scope");
   assert(strcmp(cJSON_GetObjectItem(scope, "value")->valuestring, "project-a") == 0);
   cJSON_Delete(request);

   request = cJSON_CreateObject();
   scope = cJSON_AddObjectToObject(request, "scope");
   cJSON_AddStringToObject(scope, "type", "workspace");
   cJSON_AddStringToObject(scope, "value", "explicit-workspace");
   cJSON_AddStringToObject(request, "project", "explicit-project");
   cJSON_AddBoolToObject(request, "include_all", 1);
   assert(memory_bus_add_context(request) == 0);
   assert(strcmp(cJSON_GetObjectItem(scope, "value")->valuestring, "explicit-workspace") == 0);
   assert(strcmp(cJSON_GetObjectItem(request, "project")->valuestring, "explicit-project") == 0);
   assert(cJSON_IsTrue(cJSON_GetObjectItem(request, "include_all")));
   cJSON_Delete(request);
   puts("memory bus context: inactive, inherited, and explicit scope passed");
   return 0;
}
