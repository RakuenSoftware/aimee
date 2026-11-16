/* Typed bus accessors expanded from the shared caller contract. */
#include "config.h"
#include "config_client.h"
#include "runtime_secret.h"
#include <stdio.h>

#define CONFIG_NUMBER(name, type, key, fallback)                                                   \
   type config_##name(void)                                                                        \
   {                                                                                               \
      double value = fallback;                                                                     \
      (void)config_client_read_number(key, &value);                                                \
      return (type)value;                                                                          \
   }
#define CONFIG_STRING(name, key, width)                                                            \
   const char *config_##name(void)                                                                 \
   {                                                                                               \
      static _Thread_local char value[width];                                                      \
      (void)config_client_read_string(key, value, sizeof(value));                                  \
      return value;                                                                                \
   }                                                                                               \
   size_t config_##name##_copy(char *out, size_t n)                                                \
   {                                                                                               \
      char value[width];                                                                           \
      if (!out || n == 0)                                                                          \
         return 0;                                                                                 \
      (void)config_client_read_string(key, value, sizeof(value));                                  \
      snprintf(out, n, "%s", value);                                                               \
      return sizeof(value);                                                                        \
   }
#define CONFIG_INDEX_NUMBER(name, type, key, field)                                                \
   type config_##name(int index)                                                                   \
   {                                                                                               \
      double value = 0;                                                                            \
      (void)config_client_read_indexed_number(key, index, field, &value);                          \
      return (type)value;                                                                          \
   }
#define CONFIG_INDEX_STRING(name, key, field, width)                                               \
   const char *config_##name(int index)                                                            \
   {                                                                                               \
      static _Thread_local char value[width];                                                      \
      (void)config_client_read_indexed_string(key, index, field, value, sizeof(value));            \
      return value;                                                                                \
   }
#include "config_accessor_keys.def"
#undef CONFIG_NUMBER
#undef CONFIG_STRING
#undef CONFIG_INDEX_NUMBER
#undef CONFIG_INDEX_STRING

/* Array counts and Vault-backed bearer tokens keep their explicit implementations. */
int config_mcp_client_count(void)
{
   /* mcp_clients is the documented source of truth. The process config module
    * publishes that array directly, while the legacy struct-only loader also
    * published a derived mcp_client_count scalar. Depending on the scalar makes
    * valid YAML look empty because its default remains zero. */
   cJSON *clients = config_client_value_copy("mcp_clients");
   if (cJSON_IsArray(clients))
   {
      int count = cJSON_GetArraySize(clients);
      cJSON_Delete(clients);
      return count > CONFIG_MCP_MAX_CLIENTS ? CONFIG_MCP_MAX_CLIENTS : count;
   }
   cJSON_Delete(clients);

   /* Compatibility for snapshots produced by pre-array config providers. */
   double value = 0;
   (void)config_client_read_number("mcp_client_count", &value);
   return (int)value;
}

const char *config_server_api_bearer_extra(int index)
{
   static _Thread_local char value[256];
   value[0] = 0;
   if (index >= 0 && index < AIMEE_API_BEARER_EXTRA_MAX)
   {
      char name[96];
      snprintf(name, sizeof(name), "AIMEE_API_BEARER_TOKEN_EXTRA_%d", index);
      (void)runtime_secret_get(name, value, sizeof(value));
   }
   return value;
}

int config_server_api_bearer_extra_count(void)
{
   int count = 0;
   for (; count < AIMEE_API_BEARER_EXTRA_MAX; count++)
   {
      char name[96];
      snprintf(name, sizeof(name), "AIMEE_API_BEARER_TOKEN_EXTRA_%d", count);
      if (!runtime_secret_has(name))
         break;
   }
   return count;
}
