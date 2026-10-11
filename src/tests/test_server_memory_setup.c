/* First-start writes exercise the shipping config RPC, including admin and Vault boundaries. */
#include "aimee.h"
#include "cJSON.h"
#include "config.h"
#include "config_client.h"
#include "server.h"
#include "server_http.h"
#include "server_http_identity.h"
#include "runtime_secret.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static int administrator, writes, seals, refuse_seal;
static cJSON *response, *saved;
int route_roundtable_mutation_authorized(const char *principal)
{
   (void)principal;
   return administrator;
}
const char *server_http_identity_principal(void)
{
   return administrator ? "admin" : "uid:1000";
}
int roundtable_policy_config_key(const char *key)
{
   (void)key;
   return 0;
}
int server_send_error(server_conn_t *conn, const char *message, const char *id)
{
   (void)conn;
   (void)message;
   (void)id;
   return -1;
}
int server_send_response(server_conn_t *conn, cJSON *value)
{
   (void)conn;
   cJSON_Delete(response);
   response = cJSON_Duplicate(value, 1);
   return 0;
}
int config_client_set_value(const char *key, cJSON *value)
{
   assert(!strcmp(key, "memory.backend"));
   writes++;
   cJSON_Delete(saved);
   saved = value;
   return 0;
}
int config_reload(void)
{
   return 1;
}
int config_set(const char *key, const char *value)
{
   (void)key;
   (void)value;
   assert(0);
   return -1;
}
cJSON *config_client_value_copy(const char *key)
{
   (void)key;
   return NULL;
}
cJSON *config_client_operation_response(const char *operation, cJSON *value)
{
   (void)operation;
   cJSON_Delete(value);
   assert(0);
   return NULL;
}
int config_client_key_is_secret(const char *key)
{
   return !strcmp(key, "memory_backend_token");
}
const char *config_client_secret_name(const char *key)
{
   assert(!strcmp(key, "memory_backend_token"));
   return "AIMEE_MEMORY_BACKEND_TOKEN";
}
int config_secret_store(const char *name, const char *value)
{
   assert(!strcmp(name, "AIMEE_MEMORY_BACKEND_TOKEN"));
   assert(!strcmp(value, "fixture-secret"));
   seals++;
   return refuse_seal ? -1 : 0;
}

static int set(const char *key, const char *value)
{
   cJSON *request = cJSON_CreateObject();
   cJSON_AddStringToObject(request, "key", key);
   cJSON_AddItemToObject(request, "value", cJSON_Parse(value));
   int result = handle_config_set(NULL, NULL, request);
   if (!strcmp(key, "memory_backend_token") && administrator)
      assert(cJSON_GetObjectItemCaseSensitive(request, "value")->valuestring[0] == 0);
   cJSON_Delete(request);
   return result;
}
int main(void)
{
   const char *hillock =
       "{\"engine\":\"hillock\",\"endpoint\":\"http://hillock:8097\",\"auth\":\"bearer\"}";
   assert(set("memory.backend", hillock) == -1 && writes == 0);
   assert(set("memory_backend_token", "\"fixture-secret\"") == -1 && seals == 0);
   administrator = 1;
   const char *valid[] = {
       hillock, "{\"engine\":\"native\",\"endpoint\":\"\",\"auth\":\"bearer\"}",
       "{\"engine\":\"cognee\",\"endpoint\":\"https://cognee.example/\",\"auth\":\"api-key\"}"};
   for (size_t i = 0; i < sizeof(valid) / sizeof(valid[0]); i++)
   {
      assert(set("memory.backend", valid[i]) == 0);
      assert(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(response, "applied_live")));
      assert(!strcmp(cJSON_GetObjectItemCaseSensitive(response, "reload")->valuestring, "restart"));
   }
   const char *invalid[] = {
       "{\"engine\":\"hillock\",\"endpoint\":\"http://host:invalid\",\"auth\":\"bearer\"}",
       "{\"engine\":\"hillock\",\"endpoint\":\"http://host:65536\",\"auth\":\"bearer\"}",
       "{\"engine\":\"unknown\",\"endpoint\":\"\",\"auth\":\"bearer\"}",
       "{\"engine\":\"hillock\",\"endpoint\":\"http://user:secret@host\",\"auth\":\"bearer\"}",
       "{\"engine\":\"hillock\",\"endpoint\":\"http://host/path\",\"auth\":\"bearer\"}",
       "{\"engine\":\"hillock\",\"endpoint\":\"http://host?token=x\",\"auth\":\"bearer\"}",
       "{\"engine\":\"hillock\",\"endpoint\":\"http://host\",\"auth\":\"api-key\"}",
       "{\"engine\":\"native\",\"endpoint\":\"\",\"auth\":\"bearer\",\"token\":\"secret\"}",
       "{\"engine\":false,\"endpoint\":\"\",\"auth\":\"bearer\"}"};
   for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++)
      assert(set("memory.backend", invalid[i]) == -1 && writes == 3);
   assert(set("memory.backend.engine", "\"hillock\"") == -1 && writes == 3);
   assert(set("memory.backend_token", "\"fixture-secret\"") == -1 && seals == 0);
   assert(set("memory_backend.token", "\"fixture-secret\"") == -1 && seals == 0);
   assert(set("memory_backend_endpoint", "\"http://host\"") == -1 && writes == 3);
   assert(set("memory_backend_token", "\"fixture-secret\"") == 0 && seals == 1);
   assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(response, "value")));
   assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(response, "secret")));
   refuse_seal = 1;
   assert(set("memory_backend_token", "\"fixture-secret\"") == -1 && writes == 3);
   cJSON_Delete(response);
   cJSON_Delete(saved);
   puts("server_memory_setup: OK");
   return 0;
}
