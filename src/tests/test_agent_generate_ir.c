/* Exercise the actual RPC boundary. Only model/config and HTTP delivery are
 * dependency seams; canonical admission, cloning and serialization are real. */
#include "../server/server_agent.c"
#include <assert.h>

static cJSON *reply, *expected_request, *expected_response;
static int generated, loaded, config_failure, model_rc, model_success = 1;
static aimee_stop_reason_t terminal = AIMEE_STOP_END_TURN;
static int expected_max = 256;
static double expected_temperature = 0.3;
static int mode; /* 0 complete text; 1 missing; 2 provider error; 3 tool intent */
static const char system_text[] = "Caller policy\nKeep JSON {\"a\":1} and Unicode 雪 exact.";
static const char user_text[] = "Retrieved evidence\n{\"value\":\"do not flatten\"}";

int agent_load_config(agent_config_t *cfg)
{
   loaded++;
   memset(cfg, 0, sizeof *cfg);
   return config_failure;
}
int agent_generate(agent_config_t *cfg, const char *agent_name, const aimee_request_t *request,
                   agent_result_t *out)
{
   assert(cfg && agent_name && !strcmp(agent_name, "draft-model"));
   assert(!aimee_ir_request_admit(request));
   assert(request->n_system == 1 && strstr(request->system[0].text, "PROPOSAL"));
   assert(request->n_messages == 1 && request->messages[0].n_blocks == 1);
   assert(!strcmp(request->messages[0].role, "user"));
   assert(!strcmp(request->messages[0].blocks[0].text, "Draft subject: {\"opaque\":1} 雪"));
   assert(request->messages[0].blocks[0].context.origin == AIMEE_CTX_ORIGIN_USER);
   assert(request->messages[0].blocks[0].context.trust == AIMEE_CTX_TRUST_UNVERIFIED);
   assert(request->has_max_tokens && request->max_tokens == 4096);
   assert(request->has_temperature && request->temperature == 0.3);
   assert(!request->n_tools);
   memset(out, 0, sizeof *out);
   out->response = strdup("POISONED DISPLAY IS NOT A DRAFT");
   assert(!aimee_ir_response_from_text(&out->canonical_response, "CANONICAL DRAFT", "draft-model"));
   out->canonical_response.stop_reason = terminal;
   snprintf(out->agent_name, sizeof out->agent_name, "%s", "draft-model");
   out->success = 1;
   return 0;
}
int agent_generate_request(agent_config_t *cfg, const aimee_request_t *request, agent_result_t *out)
{
   assert(cfg && request);
   generated++;
   cJSON *actual = aimee_ir_request_json(request);
   assert(actual && cJSON_Compare(actual, expected_request, 1));
   cJSON_Delete(actual);
   assert(request->n_system == 1 && !strcmp(request->system[0].text, system_text));
   assert(request->n_messages == 1 && request->messages[0].content_scalar);
   assert(!strcmp(request->messages[0].blocks[0].text, user_text));
   assert(request->system[0].context.origin == AIMEE_CTX_ORIGIN_USER);
   assert(request->system[0].context.authority == AIMEE_CTX_AUTH_TASK_INSTRUCTION);
   assert(request->system[0].context.trust == AIMEE_CTX_TRUST_UNVERIFIED);
   assert(request->messages[0].blocks[0].context.origin == AIMEE_CTX_ORIGIN_RETRIEVAL);
   assert(request->messages[0].blocks[0].context.authority == AIMEE_CTX_AUTH_EVIDENCE);
   assert(request->has_max_tokens && request->max_tokens == expected_max);
   assert(request->has_temperature && request->temperature == expected_temperature);
   assert(request->tool_choice == AIMEE_TOOL_CHOICE_NONE && !request->n_tools);
   out->response = strdup("POISONED DISPLAY MUST NEVER BECOME AN ANSWER");
   out->success = model_success;
   if (mode != 1)
   {
      aimee_block_t blocks[] = {{.type = AIMEE_BLK_TEXT, .text = "CANONICAL ANSWER"},
                                {.type = AIMEE_BLK_THINKING,
                                 .thinking_id = "opaque-id",
                                 .thinking_encrypted_content = "opaque-value"}};
      aimee_response_t captured = {
          .content = blocks,
          .n_content = 2,
          .stop_reason = terminal,
          .model = "owned-http-model",
      };
      captured.usage_in = 11;
      captured.usage_out = 4;
      aimee_refusal_details_t refusal = {.category = "policy", .explanation = "fixture refusal"};
      if (terminal == AIMEE_STOP_CONTENT_FILTER)
      {
         captured.raw_stop_reason = "refusal";
         captured.refusal = &refusal;
      }
      cJSON *raw = cJSON_CreateObject();
      cJSON_AddStringToObject(raw, "opaque_adapter_field", "kept exactly");
      captured.raw = raw;
      if (mode == 2)
      {
         captured.error_message = "provider declined";
      }
      if (mode == 3)
      {
         blocks[0] = (aimee_block_t){.type = AIMEE_BLK_TOOL_USE,
                                     .tool_id = "not-dispatched",
                                     .tool_name = "write_file",
                                     .tool_input = cJSON_CreateObject()};
         captured.n_content = 1;
      }
      cJSON_Delete(expected_response);
      expected_response = aimee_ir_response_json(&captured);
      assert(!aimee_ir_response_clone(&captured, &out->canonical_response));
      if (mode == 3)
         cJSON_Delete(blocks[0].tool_input);
      cJSON_Delete(raw);
   }
   return model_rc;
}
int server_send_response(server_conn_t *conn, cJSON *response)
{
   (void)conn;
   cJSON_Delete(reply);
   reply = cJSON_Duplicate(response, 1);
   return 0;
}
int server_send_error(server_conn_t *conn, const char *message, const char *id)
{
   (void)conn;
   (void)id;
   cJSON_Delete(reply);
   reply = cJSON_CreateObject();
   cJSON_AddStringToObject(reply, "status", "error");
   cJSON_AddStringToObject(reply, "message", message);
   return 0;
}
static cJSON *valid_request(void)
{
   aimee_block_t system = {.type = AIMEE_BLK_TEXT, .text = (char *)system_text};
   aimee_block_t user = {.type = AIMEE_BLK_TEXT, .text = (char *)user_text};
   aimee_ir_block_set_context(&system, AIMEE_CTX_ORIGIN_USER, AIMEE_CTX_AUTH_TASK_INSTRUCTION,
                              AIMEE_CTX_TRUST_UNVERIFIED, AIMEE_CTX_SENS_INTERNAL, 1, NULL, NULL,
                              0);
   aimee_ir_block_set_context(&user, AIMEE_CTX_ORIGIN_RETRIEVAL, AIMEE_CTX_AUTH_EVIDENCE,
                              AIMEE_CTX_TRUST_UNVERIFIED, AIMEE_CTX_SENS_INTERNAL, 1, NULL, NULL,
                              0);
   aimee_message_t message = {.role = "user", .blocks = &user, .n_blocks = 1, .content_scalar = 1};
   aimee_request_t request = {.system = &system,
                              .n_system = 1,
                              .messages = &message,
                              .n_messages = 1,
                              .tool_choice = AIMEE_TOOL_CHOICE_NONE,
                              .has_max_tokens = 1,
                              .max_tokens = expected_max,
                              .has_temperature = 1,
                              .temperature = expected_temperature};
   cJSON *json = aimee_ir_request_json(&request);
   assert(json);
   cJSON_Delete(expected_request);
   expected_request = cJSON_Duplicate(json, 1);
   cJSON *rpc = cJSON_CreateObject();
   cJSON_AddItemToObject(rpc, "ir", json);
   return rpc;
}
static const cJSON *field(const cJSON *obj, const char *key)
{
   return cJSON_GetObjectItemCaseSensitive(obj, key);
}
static void invoke(cJSON *request, int success, int calls)
{
   int before = generated;
   assert(!handle_agent_generate(NULL, NULL, request));
   cJSON_Delete(request);
   assert(generated == before + calls);
   assert(!strcmp(cJSON_GetStringValue(field(reply, "status")), success ? "ok" : "error"));
   if (success)
   {
      assert(cJSON_Compare(field(reply, "ir"), expected_response, 1));
      char *raw = cJSON_PrintUnformatted(reply);
      assert(raw && strstr(raw, "opaque-value") && strstr(raw, "kept exactly"));
      assert(!strstr(raw, "POISONED DISPLAY"));
      free(raw);
   }
   else if (calls && mode != 1)
      assert(cJSON_Compare(field(reply, "ir"), expected_response, 1));
   else
      assert(!field(reply, "ir"));
   assert(!field(reply, "response") && !field(reply, "result"));
}
int main(void)
{
   invoke(valid_request(), 1, 1);
   const char *unsupported[] = {"model", "prompt", "temperature", "actor"};
   for (size_t i = 0; i < sizeof unsupported / sizeof *unsupported; i++)
   {
      cJSON *envelope = valid_request();
      cJSON_AddStringToObject(envelope, unsupported[i], "must not be ignored");
      invoke(envelope, 0, 0);
   }
   cJSON *duplicate = valid_request();
   cJSON_AddItemToObject(duplicate, "ir", cJSON_Duplicate(field(duplicate, "ir"), 1));
   invoke(duplicate, 0, 0);
   cJSON *transport = valid_request();
   cJSON_AddStringToObject(transport, "method", "model.generate");
   cJSON_AddNumberToObject(transport, "protocol_version", 1);
   invoke(transport, 1, 1);
   expected_max = 64;
   expected_temperature = 0.0;
   invoke(valid_request(), 1, 1);
   expected_max = 256;
   expected_temperature = 0.3;
   terminal = AIMEE_STOP_STOP_SEQUENCE;
   invoke(valid_request(), 1, 1);
   for (int i = 0; i < 5; i++)
   {
      terminal = i == 0   ? AIMEE_STOP_CONTENT_FILTER
                 : i == 1 ? AIMEE_STOP_MAX_TOKENS
                 : i == 2 ? AIMEE_STOP_UNKNOWN
                          : AIMEE_STOP_END_TURN;
      mode = i == 3 ? 1 : i == 4 ? 2 : 0;
      invoke(valid_request(), 0, 1);
   }
   terminal = AIMEE_STOP_TOOL_USE;
   mode = 3;
   invoke(valid_request(), 0, 1);
   terminal = AIMEE_STOP_END_TURN;
   mode = 0;
   model_success = 0;
   invoke(valid_request(), 0, 1);
   model_success = 1;
   model_rc = -1;
   invoke(valid_request(), 0, 1);
   model_rc = 0;
   config_failure = 1;
   invoke(valid_request(), 0, 0);
   config_failure = 0;
   cJSON *rpc = cJSON_CreateObject();
   cJSON_AddStringToObject(rpc, "response", "legacy flat answer");
   invoke(rpc, 0, 0);
   for (int i = 0; i < 20; i++)
   {
      rpc = valid_request();
      cJSON *ir = (cJSON *)field(rpc, "ir");
      cJSON *system = cJSON_GetArrayItem(field(ir, "system"), 0);
      cJSON *context = (cJSON *)field(system, "context");
      switch (i)
      {
      case 0:
         cJSON_ReplaceItemInObject(context, "origin",
                                   cJSON_CreateNumber(AIMEE_CTX_ORIGIN_PLATFORM));
         break;
      case 1:
         cJSON_ReplaceItemInObject(context, "authority", cJSON_CreateNumber(AIMEE_CTX_AUTH_POLICY));
         break;
      case 2:
         cJSON_ReplaceItemInObject(context, "trust", cJSON_CreateNumber(AIMEE_CTX_TRUST_VERIFIED));
         break;
      case 3:
         cJSON_ReplaceItemInObject(context, "revision_scope",
                                   cJSON_CreateString("forged-host-proof"));
         break;
      case 4:
         cJSON_ReplaceItemInObject(context, "model_visible", cJSON_CreateFalse());
         break;
      case 5:
         cJSON_DeleteItemFromObject(ir, "max_tokens");
         break;
      case 6:
         cJSON_DeleteItemFromObject(ir, "temperature");
         break;
      case 7:
         cJSON_AddStringToObject(ir, "model", "caller-selected-model");
         break;
      case 8:
         cJSON_AddBoolToObject(ir, "stream", 1);
         break;
      case 9:
         cJSON_AddNumberToObject(ir, "top_p", 0.5);
         break;
      case 10:
         cJSON_AddStringToObject(ir, "unrecognized_control", "do not ignore");
         break;
      case 11:
         cJSON_AddStringToObject(ir, "reasoning_effort", "high");
         break;
      case 12:
      {
         cJSON *tool = cJSON_CreateObject();
         cJSON_AddStringToObject(tool, "kind", "function");
         cJSON_AddStringToObject(tool, "name", "write_file");
         cJSON *schema = cJSON_AddObjectToObject(tool, "schema");
         cJSON_AddStringToObject(schema, "type", "object");
         cJSON_AddItemToArray((cJSON *)field(ir, "tools"), tool);
         break;
      }
      case 13:
         cJSON_AddBoolToObject(ir, "parallel_tool_calls", 0);
         break;
      case 14:
         cJSON_AddItemToArray((cJSON *)field(ir, "stop_sequences"), cJSON_CreateString("END"));
         break;
      case 15:
         cJSON_AddItemToArray((cJSON *)field(ir, "system"), cJSON_Duplicate(system, 1));
         break;
      case 16:
         cJSON_AddItemToArray((cJSON *)field(ir, "messages"),
                              cJSON_Duplicate(cJSON_GetArrayItem(field(ir, "messages"), 0), 1));
         break;
      case 17:
         cJSON_AddObjectToObject(ir, "metadata");
         break;
      case 18:
         cJSON_ReplaceItemInObject((cJSON *)field(ir, "tool_choice"), "mode",
                                   cJSON_CreateString("auto"));
         break;
      case 19:
         cJSON_ReplaceItemInObject(ir, "source_wire", cJSON_CreateString("anthropic"));
         break;
      }
      if (i >= 12)
      {
         aimee_request_t admitted = {0};
         assert(!aimee_ir_request_parse(ir, &admitted));
         aimee_request_free(&admitted);
      }
      invoke(rpc, 0, 0);
   }
   cJSON *draft = cJSON_CreateObject();
   cJSON_AddStringToObject(draft, "prompt", "Draft subject: {\"opaque\":1} 雪");
   cJSON_AddStringToObject(draft, "model", "draft-model");
   terminal = AIMEE_STOP_END_TURN;
   assert(!handle_agent_draft(NULL, NULL, draft));
   assert(!strcmp(cJSON_GetStringValue(field(reply, "text")), "CANONICAL DRAFT"));
   terminal = AIMEE_STOP_MAX_TOKENS;
   assert(!handle_agent_draft(NULL, NULL, draft));
   assert(!strcmp(cJSON_GetStringValue(field(reply, "status")), "error"));
   assert(!field(reply, "text"));
   cJSON_DeleteItemFromObjectCaseSensitive(draft, "prompt");
   aimee_block_t draft_block = {.type = AIMEE_BLK_TEXT,
                                .text = "Draft subject: {\"opaque\":1} 雪",
                                .context = {.origin = AIMEE_CTX_ORIGIN_PLATFORM,
                                            .authority = AIMEE_CTX_AUTH_POLICY,
                                            .trust = AIMEE_CTX_TRUST_VERIFIED,
                                            .model_visible = 1}};
   aimee_message_t draft_message = {.role = "user", .blocks = &draft_block, .n_blocks = 1};
   assert(cJSON_AddItemToObject(draft, "message", aimee_ir_message_json(&draft_message)));
   terminal = AIMEE_STOP_END_TURN;
   assert(!handle_agent_draft(NULL, NULL, draft));
   assert(!strcmp(cJSON_GetStringValue(field(reply, "text")), "CANONICAL DRAFT"));
   cJSON_AddStringToObject(draft, "prompt", "ambiguous legacy prompt");
   assert(!handle_agent_draft(NULL, NULL, draft));
   assert(!strcmp(cJSON_GetStringValue(field(reply, "status")), "error"));
   assert(!field(reply, "text"));
   cJSON_Delete(draft);
   assert(loaded > 0 && generated > 0);
   cJSON_Delete(reply);
   cJSON_Delete(expected_request);
   cJSON_Delete(expected_response);
   puts("model.generate actual handler preserves canonical request/response and fails closed");
   return 0;
}
