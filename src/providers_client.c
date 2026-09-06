/* Compatibility transport from the C resource plane to the Go providers owner. */
#include "providers_client.h"
#include "module_json_call.h"

cJSON *providers_module_request(const char *operation, cJSON *arguments, const char *actor,
                                int secret_write_allowed)
{
   cJSON *wire = cJSON_CreateObject();
   cJSON_AddStringToObject(wire, "operation", operation);
   cJSON_AddItemToObject(wire, "arguments",
                         arguments ? cJSON_Duplicate(arguments, 1) : cJSON_CreateObject());
   cJSON_AddStringToObject(wire, "actor", actor ? actor : "");
   cJSON_AddBoolToObject(wire, "secret_write_allowed", secret_write_allowed);
   return aimee_module_json_call(12547u, 3u, wire, 1024u * 1024u, 30000, NULL);
}
