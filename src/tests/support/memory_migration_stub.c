/* Narrow test doubles for legacy KB_STORE callers whose memory providers moved to Go. */
#include "aimee.h"

#include <stdio.h>

/* Unconfigured module transport for legacy KB_STORE-only fixtures. Consumer-specific
 * tests supply a strong transport mock and assert the Go response contract. */
__attribute__((weak)) int aimee_module_commands_dispatch_internal(const char *method,
                                                                  const cJSON *args, cJSON **result)
{
   (void)args;
   if (strcmp(method, "memory.runtime") != 0)
      return 0;
   *result = cJSON_CreateObject();
   cJSON_AddStringToObject(*result, "status", "error");
   cJSON_AddStringToObject(*result, "kind", "not_found");
   return 1;
}

__attribute__((weak)) int aimee_module_commands_dispatch_internal_timeout(const char *method,
                                                                          const cJSON *args,
                                                                          int timeout_ms,
                                                                          cJSON **result)
{
   (void)timeout_ms;
   return aimee_module_commands_dispatch_internal(method, args, result);
}

/* Legacy-only consumer fixtures have no installed verifier context factory. */
__attribute__((weak)) int aimee_module_commands_dispatch_internal_caller(const char *method,
                                                                         const cJSON *args,
                                                                         cJSON **result)
{
   return aimee_module_commands_dispatch_internal(method, args, result);
}
__attribute__((weak)) int aimee_module_commands_dispatch_internal_caller_timeout(const char *method,
                                                                                 const cJSON *args,
                                                                                 int timeout_ms,
                                                                                 cJSON **result)
{
   return aimee_module_commands_dispatch_internal_timeout(method, args, timeout_ms, result);
}
