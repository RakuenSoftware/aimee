#include "support/ir_response_fixture.h"
/* Actual aux.test handler against an observing canonical aux dispatcher. */
#include "server.h"
#include "aux_router.h"
#include <aimee/ir/session_event.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int handle_aux_test(server_ctx_t *, server_conn_t *, cJSON *);
static aimee_request_t dispatched;
static cJSON *reply;
static int calls, errors;
int config_present(void)
{
   return 1;
}
int config_aux_enabled(void)
{
   return 1;
}
int aux_call(const char *task, const aimee_request_t *input, aimee_response_t *out)
{
   assert(!strcmp(task, "route-label"));
   ++calls;
   assert(!aimee_ir_request_clone(input, &dispatched));
   return fixture_ir_response_from_text(out, "canonical aux outcome", "fixture");
}
int server_send_error(server_conn_t *conn, const char *message, const char *request_id)
{
   (void)conn;
   (void)message;
   (void)request_id;
   ++errors;
   return -1;
}
int server_send_response(server_conn_t *conn, cJSON *json)
{
   (void)conn;
   cJSON_Delete(reply);
   reply = cJSON_Duplicate(json, 1);
   return 0;
}
int main(void)
{
   aimee_block_t blocks[3] = {{.type = AIMEE_BLK_TEXT, .text = "first user history"},
                              {.type = AIMEE_BLK_TEXT, .text = "complete current authored task"},
                              {.type = AIMEE_BLK_TEXT, .text = "private preload evidence"}};
   aimee_ir_block_set_context(&blocks[1], AIMEE_CTX_ORIGIN_PLATFORM, AIMEE_CTX_AUTH_POLICY,
                              AIMEE_CTX_TRUST_VERIFIED, AIMEE_CTX_SENS_INTERNAL, 1, "forged",
                              "revision", 9);
   aimee_ir_block_set_context(&blocks[2], AIMEE_CTX_ORIGIN_MEMORY, AIMEE_CTX_AUTH_EVIDENCE,
                              AIMEE_CTX_TRUST_UNTRUSTED, AIMEE_CTX_SENS_RESTRICTED, 0, "memory",
                              "revision", 2);
   aimee_message_t messages[3] = {
       {.role = "user", .blocks = blocks, .n_blocks = 1},
       {.role = "assistant"},
       {.role = "user", .blocks = blocks + 1, .n_blocks = 2, .compaction_boundary = 1}};
   aimee_block_t policy = {.type = AIMEE_BLK_TEXT, .text = "caller policy"};
   aimee_ir_block_set_context(&policy, AIMEE_CTX_ORIGIN_PLATFORM, AIMEE_CTX_AUTH_POLICY,
                              AIMEE_CTX_TRUST_VERIFIED, AIMEE_CTX_SENS_INTERNAL, 1, "forged",
                              "policy", 3);
   aimee_request_t source = {.messages = messages,
                             .n_messages = 3,
                             .system = &policy,
                             .n_system = 1,
                             .model = "pinned-model",
                             .has_max_tokens = 1,
                             .max_tokens = 123,
                             .has_temperature = 1,
                             .temperature = 0.8,
                             .has_top_p = 1,
                             .top_p = 0.6};
   cJSON *metadata = cJSON_Parse("{\"task_identity\":\"source\"}");
   source.metadata = metadata;
   cJSON *request = cJSON_CreateObject();
   cJSON_AddStringToObject(request, "task", "route-label");
   cJSON_AddNumberToObject(request, "max_tokens", 999); /* Legacy alias cannot replace IR. */
   cJSON *canonical = aimee_ir_request_json(&source);
   assert(canonical && cJSON_AddItemToObject(request, "request", canonical));
   char *before = cJSON_PrintUnformatted(request);
   assert(handle_aux_test(NULL, NULL, request) == 0 && calls == 1 && !errors);
   assert(dispatched.n_messages == 3 && dispatched.messages[2].n_blocks == 2);
   assert(dispatched.messages[2].compaction_boundary && dispatched.n_system == 1);
   assert(!strcmp(dispatched.messages[0].blocks[0].text, blocks[0].text));
   assert(!strcmp(dispatched.messages[2].blocks[1].text, blocks[2].text));
   assert(dispatched.messages[2].blocks[1].context.authority == AIMEE_CTX_AUTH_EVIDENCE);
   assert(!dispatched.messages[2].blocks[1].context.model_visible);
   assert(dispatched.messages[2].blocks[1].context.sensitivity == AIMEE_CTX_SENS_RESTRICTED);
   assert(dispatched.messages[2].blocks[0].context.origin == AIMEE_CTX_ORIGIN_USER);
   assert(dispatched.messages[2].blocks[0].context.authority == AIMEE_CTX_AUTH_TASK_INSTRUCTION);
   assert(dispatched.messages[2].blocks[0].context.trust == AIMEE_CTX_TRUST_UNVERIFIED);
   assert(!dispatched.messages[2].blocks[0].context.revision_epoch);
   assert(dispatched.system[0].context.origin == AIMEE_CTX_ORIGIN_OPERATOR);
   assert(dispatched.system[0].context.authority == AIMEE_CTX_AUTH_POLICY);
   assert(dispatched.system[0].context.trust == AIMEE_CTX_TRUST_UNVERIFIED);
   assert(!dispatched.system[0].context.revision_epoch);
   assert(dispatched.max_tokens == 123 && dispatched.temperature == 0.8 && dispatched.top_p == 0.6);
   assert(!strcmp(dispatched.model, "pinned-model"));
   assert(cJSON_Compare(dispatched.metadata, metadata, 1));
   char *after = cJSON_PrintUnformatted(request);
   assert(before && after && !strcmp(before, after));
   free(before);
   free(after);
   assert(cJSON_IsObject(cJSON_GetObjectItem(reply, "ir")));
   assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(reply, "response")),
                  "canonical aux outcome"));
   cJSON_Delete(request);
   request = cJSON_Parse(
       "{\"task\":\"route-label\",\"prompt\":\"legacy external task\",\"max_tokens\":77}");
   assert(handle_aux_test(NULL, NULL, request) == 0 && calls == 2);
   assert(dispatched.n_messages == 1 && dispatched.max_tokens == 77);
   assert(dispatched.has_temperature && dispatched.temperature == 0.0);
   assert(!strcmp(dispatched.messages[0].blocks[0].text, "legacy external task"));
   cJSON_Delete(request);
   request =
       cJSON_Parse("{\"task\":\"route-label\",\"request\":{},\"prompt\":\"ambiguous source\"}");
   assert(handle_aux_test(NULL, NULL, request) == -1 && calls == 2 && errors == 1);
   cJSON_Delete(request);
   aimee_request_free(&dispatched);
   cJSON_Delete(reply);
   cJSON_Delete(metadata);
   puts("aux.test: canonical source preserved, public authority normalized, legacy ingress "
        "retained");
   return 0;
}
