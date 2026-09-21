/* Narrow test doubles for legacy DB2 callers whose memory providers moved to Go. */
#include "aimee.h"

#include <stdio.h>

int64_t db2_memory_count(void)
{
   return 0;
}

int db2_memory_count_l2(void)
{
   return 0;
}

int db2_memory_count_l3(void)
{
   return 0;
}

int db2_memory_count_orphaned_l0(void)
{
   return 0;
}

/* Unconfigured module transport for legacy DB2-only fixtures. Consumer-specific
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
