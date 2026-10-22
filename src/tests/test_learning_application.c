/* Host transport cannot turn a caller envelope into authenticated exposure. */
#include "aimee.h"
#include "server.h"
#include "kb_client.h"
#include "module_json_call.h"
#include "request_context.h"
#include "server_learning_application.h"
#include "json_fluent.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

static cJSON *ledger, *captured, *reply;
static int writes, local_writes, receipts, configured = 1;
static char kb_mode[16] = "remote";
size_t config_kb_mode_copy(char *out, size_t capacity)
{
   snprintf(out, capacity, "%s", kb_mode);
   return strlen(kb_mode);
}
int kb_client_connection_configured(void)
{
   return configured;
}
cJSON *server_learning_application_local(const request_context_t *owner, const cJSON *event,
                                         const cJSON *exposure, const char *scope_kind,
                                         const char *scope_id)
{
   assert(owner && !owner->model_execution && !strcmp(owner->vault_principal, "uid:test"));
   local_writes++;
   cJSON_Delete(captured);
   captured = cJSON_CreateObject();
   cJSON_AddItemToObject(captured, "governed_event", cJSON_Duplicate(event, 1));
   cJSON_AddItemToObject(captured, "exposure", cJSON_Duplicate(exposure, 1));
   cJSON_AddStringToObject(captured, "scope_kind", scope_kind);
   cJSON_AddStringToObject(captured, "scope_id", scope_id);
   return cJSON_Parse("{\"status\":\"ok\",\"store\":\"local\"}");
}
int send_and_free(server_conn_t *conn, cJSON *value)
{
   (void)conn;
   cJSON_Delete(reply);
   reply = value;
   return 0;
}
int server_send_error(server_conn_t *conn, const char *message, const char *request_id)
{
   (void)conn;
   (void)message;
   (void)request_id;
   cJSON_Delete(reply);
   reply = cJSON_Parse("{\"status\":\"error\"}");
   return -1;
}
cJSON *ingress_preinject_receipt(const char *id)
{
   receipts++;
   assert(!strcmp(id, "request"));
   return cJSON_Duplicate(ledger, 1);
}
char *kb_client_learning_application_json(cJSON *request)
{
   ++writes;
   cJSON_Delete(captured);
   captured = request;
   return strdup("{\"status\":\"ok\",\"event_ref\":\"governed\"}");
}
cJSON *aimee_module_json_call(uint32_t kind, uint32_t stage, cJSON *request, size_t max,
                              int timeout, aimee_module_call_result_t *result)
{
   (void)max;
   (void)timeout;
   (void)result;
   assert(kind == 11017 && stage == 9);
   cJSON_Delete(captured);
   captured = request;
   return cJSON_Parse("{\"known_total_nanodollars\":\"9007199254740993\",\"policy_admission\":"
                      "\"observe_only_unqualified\"}");
}
int main(void)
{
   request_context_t owner = {.vault_transport = ATTEST_UDS_PEERCRED};
   strcpy(owner.vault_principal, "uid:test");
   request_context_set(&owner);
   ledger = cJSON_Parse(
       "{\"status\":\"ok\",\"receipts\":[{\"attempt_id\":\"a\",\"prepared_receipt\":{\"binding\":{"
       "\"project\":\"bound-project\"}},\"procedure_exposures\":[{\"receipt_ref\":\"bound\","
       "\"procedure\":{\"owner_id\":\"kb\",\"procedure_id\":\"9007199254740993\",\"revision\":"
       "\"2\"},\"state\":\"delivered\",\"producer\":\"local_host_ledger\"}]}]}");
   cJSON *request = cJSON_Parse(
       "{\"request_id\":\"request\",\"scope_id\":\"forged-project\",\"exposure\":{\"receipt_ref\":"
       "\"forged\"},\"event\":{\"attempt_id\":\"a\",\"receipt_ref\":\"bound\",\"procedure\":{"
       "\"owner_id\":\"kb\",\"procedure_id\":\"9007199254740993\",\"revision\":\"2\"}}}");
   assert(handle_learning_application(NULL, NULL, request) == 0 && writes == 1);
   assert(!strcmp(jo_str(captured, "scope_id", ""), "bound-project"));
   assert(!strcmp(jo_str(cJSON_GetObjectItem(captured, "exposure"), "receipt_ref", ""), "bound"));
   strcpy(kb_mode, "none");
   assert(handle_learning_application(NULL, NULL, request) == 0 && writes == 1 &&
          local_writes == 1);
   assert(!strcmp(jo_str(captured, "scope_id", ""), "bound-project"));
   assert(!strcmp(jo_str(captured, "scope_kind", ""), "project"));
   assert(!strcmp(jo_str(cJSON_GetObjectItem(captured, "exposure"), "receipt_ref", ""), "bound"));
   kb_mode[0] = 0;
   configured = 0;
   assert(handle_learning_application(NULL, NULL, request) == 0 && writes == 1 &&
          local_writes == 2);
   strcpy(kb_mode, "remote");
   assert(handle_learning_application(NULL, NULL, request) == 0 && writes == 2 &&
          local_writes == 2);
   owner.model_execution = 1;
   request_context_set(&owner);
   int before = receipts;
   assert(handle_learning_application(NULL, NULL, request) == -1 && receipts == before &&
          writes == 2 && local_writes == 2);
   owner.model_execution = 0;
   request_context_set(&owner);
   cJSON *event = cJSON_GetObjectItem(request, "event");
   cJSON_ReplaceItemInObject(event, "receipt_ref", cJSON_CreateString("forged"));
   assert(handle_learning_application(NULL, NULL, request) == -1 && writes == 2);
   cJSON_ReplaceItemInObject(event, "receipt_ref", cJSON_CreateString("bound"));
   cJSON_ReplaceItemInObject(cJSON_GetObjectItem(event, "procedure"), "revision",
                             cJSON_CreateString("3"));
   assert(handle_learning_application(NULL, NULL, request) == -1 && writes == 2);
   cJSON_Delete(ledger);
   ledger = NULL;
   assert(handle_learning_application(NULL, NULL, request) == -1 && writes == 2);
   cJSON_Delete(request);
   request = cJSON_Parse("{\"cost_json\":\"{\\\"schema_version\\\":1,\\\"nanodollars\\\":"
                         "\\\"9007199254740993\\\"}\"}");
   assert(handle_learning_task_cost(NULL, NULL, request) == 0);
   assert(!strcmp(jo_str(captured, "nanodollars", ""), "9007199254740993"));
   cJSON_Delete(request);
   cJSON_Delete(reply);
   cJSON_Delete(captured);
   request_context_clear();
   return 0;
}
