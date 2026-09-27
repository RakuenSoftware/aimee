/* Host transport cannot turn a caller envelope into authenticated exposure. */
#include "aimee.h"
#include "server.h"
#include "kb_client.h"
#include "module_json_call.h"
#include "json_fluent.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

static cJSON *ledger, *captured, *reply;
static int writes;
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
   cJSON *event = cJSON_GetObjectItem(request, "event");
   cJSON_ReplaceItemInObject(event, "receipt_ref", cJSON_CreateString("forged"));
   assert(handle_learning_application(NULL, NULL, request) == -1 && writes == 1);
   cJSON_ReplaceItemInObject(event, "receipt_ref", cJSON_CreateString("bound"));
   cJSON_ReplaceItemInObject(cJSON_GetObjectItem(event, "procedure"), "revision",
                             cJSON_CreateString("3"));
   assert(handle_learning_application(NULL, NULL, request) == -1 && writes == 1);
   cJSON_Delete(ledger);
   ledger = NULL;
   assert(handle_learning_application(NULL, NULL, request) == -1 && writes == 1);
   cJSON_Delete(request);
   request = cJSON_Parse("{\"cost_json\":\"{\\\"schema_version\\\":1,\\\"nanodollars\\\":"
                         "\\\"9007199254740993\\\"}\"}");
   assert(handle_learning_task_cost(NULL, NULL, request) == 0);
   assert(!strcmp(jo_str(captured, "nanodollars", ""), "9007199254740993"));
   cJSON_Delete(request);
   cJSON_Delete(reply);
   cJSON_Delete(captured);
   return 0;
}
