/* The native API is a stateless transport; policy and lifecycle cases live in
 * server-go/modules/providers. Pin attestation, forwarding and typed failures. */
#include "server.h"
#include "providers_client.h"
#include "vault_capability.h"
#include "cJSON.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static cJSON *reply, *sent, *arguments;
static char operation[80], actor[128], error_kind[64];
static int secret_allowed;
cJSON *providers_module_request(const char *op, cJSON *args, const char *principal, int allowed)
{
   snprintf(operation, sizeof(operation), "%s", op);
   snprintf(actor, sizeof(actor), "%s", principal ? principal : "");
   secret_allowed = allowed;
   cJSON_Delete(arguments);
   arguments = cJSON_Duplicate(args, 1);
   return cJSON_Duplicate(reply, 1);
}
int vault_agent_key_server_seal_allowed(attested_transport_t transport)
{
   return transport == ATTEST_TLS_BEARER;
}
int server_send_response(server_conn_t *conn, cJSON *response)
{
   (void)conn;
   cJSON_Delete(sent);
   sent = cJSON_Duplicate(response, 1);
   return 0;
}
int server_send_error_kind(server_conn_t *conn, const char *kind, const char *message,
                           const char *id)
{
   (void)conn;
   (void)message;
   (void)id;
   snprintf(error_kind, sizeof(error_kind), "%s", kind);
   return 0;
}
int main(void)
{
   server_conn_t conn = {0};
   snprintf(conn.vault_principal, sizeof(conn.vault_principal), "webuser:tester");
   reply = cJSON_Parse("{\"status\":\"ok\",\"providers\":[{\"name\":\"work\"}]}");
   cJSON *request = cJSON_Parse("{\"name\":\"work\",\"create\":true}");
   handle_provider_save_connection(NULL, &conn, request);
   assert(!strcmp(operation, "provider.save_connection"));
   assert(!strcmp(actor, "webuser:tester"));
   assert(!secret_allowed);
   assert(cJSON_IsTrue(cJSON_GetObjectItem(arguments, "create")));
   conn.attested_transport = ATTEST_TLS_BEARER;
   handle_provider_save_connection(NULL, &conn, request);
   assert(secret_allowed);
   handle_provider_remove_connection(NULL, &conn, request);
   assert(!strcmp(operation, "provider.remove_connection"));
   handle_agent_set(NULL, &conn, request);
   assert(!strcmp(operation, "model.set"));
   handle_agent_list(NULL, &conn, request);
   assert(!strcmp(operation, "model.list"));
   cJSON_Delete(reply);
   reply = NULL;
   handle_agent_list(NULL, &conn, request);
   assert(!strcmp(error_kind, "unavailable"));
   reply = cJSON_Parse(
       "{\"status\":\"error\",\"kind\":\"not_found\",\"message\":\"provider not found\"}");
   handle_provider_remove_connection(NULL, &conn, request);
   assert(!strcmp(error_kind, "not_found"));
   cJSON_Delete(reply);
   cJSON_Delete(request);
   cJSON_Delete(sent);
   cJSON_Delete(arguments);
   puts("provider API transport: PASS");
   return 0;
}
