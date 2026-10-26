/* Exercise the actual RPC boundary. Only model/config and HTTP delivery are
 * dependency seams; canonical admission, cloning and serialization are real. */
#include "../server/server_agent.c"
#include <assert.h>

static cJSON *reply, *expected_request, *expected_response;
static int generated, loaded, config_failure, model_rc, model_success = 1;
static aimee_stop_reason_t terminal = AIMEE_STOP_END_TURN;
static int expected_max = 256;
static double expected_temperature = 0.3;
static const aimee_context_meta_t *expected_draft_context;
static const char *expected_alias = "default-http";
static int mode; /* 0 complete text; 1 missing; 2 provider error; 3 tool intent */
static const char system_text[] = "Caller policy\nKeep JSON {\"a\":1} and Unicode 雪 exact.";
static const char user_text[] = "Retrieved evidence\n{\"value\":\"do not flatten\"}";

int agent_load_config(agent_config_t *cfg)
{
   loaded++;
   memset(cfg, 0, sizeof *cfg);
   cfg->agent_count = 4;
   const char *names[] = {"default-http", "discord-e2b", "native-cli", "disabled-http"};
   for (int i = 0; i < 4; i++)
   {
      snprintf(cfg->agents[i].name, sizeof cfg->agents[i].name, "%s", names[i]);
      cfg->agents[i].enabled = i != 3;
   }
   snprintf(cfg->agents[2].backend, sizeof cfg->agents[2].backend, "%s",
            AGENT_BACKEND_PROVIDER_CLI);
   snprintf(cfg->default_agent, sizeof cfg->default_agent, "default-http");
   return config_failure;
}
agent_t *agent_find(agent_config_t *cfg, const char *name)
{
   for (int i = 0; i < cfg->agent_count; i++)
      if (!strcmp(name, cfg->agents[i].name))
         return &cfg->agents[i];
   return NULL;
}
int agent_uses_provider_cli(const agent_t *agent)
{
   return agent->backend[0] != 0;
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
   if (expected_draft_context)
      assert(!memcmp(&request->messages[0].blocks[0].context, expected_draft_context,
                      sizeof(*expected_draft_context)));
   else
   {
      assert(request->messages[0].blocks[0].context.origin == AIMEE_CTX_ORIGIN_USER);
      assert(request->messages[0].blocks[0].context.trust == AIMEE_CTX_TRUST_UNVERIFIED);
   }
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
   assert(!strcmp(cfg->default_agent, expected_alias));
   if (request->n_messages == 3)
   {
      assert(request->n_system == 2 && request->messages[2].n_blocks == 2);
      assert(request->system[1].context.origin == AIMEE_CTX_ORIGIN_MEMORY);
      assert(request->system[1].context.authority == AIMEE_CTX_AUTH_EVIDENCE);
      assert(request->system[1].context.revision_epoch == 7);
      assert(request->system[1].context.sensitivity == AIMEE_CTX_SENS_CONFIDENTIAL);
      assert(!request->messages[1].blocks[1].context.model_visible);
      assert(!strcmp(request->messages[1].blocks[1].thinking_encrypted_content, "opaque-cipher"));
      assert(request->messages[2].blocks[0].context.origin == AIMEE_CTX_ORIGIN_USER);
      assert(request->messages[2].blocks[0].context.authority == AIMEE_CTX_AUTH_TASK_INSTRUCTION);
      assert(!request->messages[2].blocks[0].context.revision_epoch);
      char *metadata = cJSON_PrintUnformatted(request->metadata);
      assert(metadata && strstr(metadata, "9007199254740993"));
      free(metadata);
   }
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
/* These fixtures supply caller-authored system policy. Expected normalization
 * is independently stated, while all other body/control bytes remain exact. */
static void expect_caller_policy(cJSON *ir)
{
   cJSON_Delete(expected_request);
   expected_request = cJSON_Duplicate(ir, 1);
   cJSON *blocks = cJSON_GetObjectItemCaseSensitive(expected_request, "system"), *block;
   cJSON_ArrayForEach(block, blocks)
   {
      cJSON *context = cJSON_GetObjectItemCaseSensitive(block, "context");
      cJSON_ReplaceItemInObject(context, "origin", cJSON_CreateNumber(AIMEE_CTX_ORIGIN_OPERATOR));
      cJSON_ReplaceItemInObject(context, "authority", cJSON_CreateNumber(AIMEE_CTX_AUTH_POLICY));
      cJSON_ReplaceItemInObject(context, "trust", cJSON_CreateNumber(AIMEE_CTX_TRUST_UNVERIFIED));
      cJSON_ReplaceItemInObject(context, "revision_domain", cJSON_CreateString(""));
      cJSON_ReplaceItemInObject(context, "revision_scope", cJSON_CreateString(""));
      cJSON_ReplaceItemInObject(context, "revision_epoch", cJSON_CreateString("0"));
   }
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
   expect_caller_policy(json);
   cJSON *rpc = cJSON_CreateObject();
   cJSON_AddItemToObject(rpc, "ir", json);
   return rpc;
}
static cJSON *rich_request(void)
{
   cJSON *rpc = valid_request(), *ir = cJSON_GetObjectItemCaseSensitive(rpc, "ir");
   aimee_block_t memory = {.type = AIMEE_BLK_TEXT, .text = "Memory: JSON {\"source\":17} 雪"};
   aimee_ir_block_set_context(&memory, AIMEE_CTX_ORIGIN_MEMORY, AIMEE_CTX_AUTH_EVIDENCE,
                              AIMEE_CTX_TRUST_UNVERIFIED, AIMEE_CTX_SENS_CONFIDENTIAL, 1, "memory",
                              "note:17", 7);
   /* The same evidence is deliberately legal in system: its authority and
    * revision custody must survive public admission without becoming policy. */
   aimee_message_t evidence_view = {.role = "user", .blocks = &memory, .n_blocks = 1};
   cJSON *view = aimee_ir_message_json(&evidence_view);
   cJSON *block = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(view, "blocks"), 0);
   cJSON_AddItemToArray(cJSON_GetObjectItemCaseSensitive(ir, "system"), cJSON_Duplicate(block, 1));
   cJSON_AddItemToArray(cJSON_GetObjectItemCaseSensitive(expected_request, "system"),
                        cJSON_Duplicate(block, 1));
   cJSON_Delete(view);
   aimee_block_t prior[] = {{.type = AIMEE_BLK_TEXT, .text = "prior assistant"},
                            {.type = AIMEE_BLK_THINKING,
                             .text = "hidden reasoning",
                             .thinking_id = "opaque-id",
                             .thinking_encrypted_content = "opaque-cipher"}};
   aimee_ir_block_set_context(&prior[0], AIMEE_CTX_ORIGIN_MODEL, AIMEE_CTX_AUTH_EVIDENCE,
                              AIMEE_CTX_TRUST_UNVERIFIED, AIMEE_CTX_SENS_INTERNAL, 1, NULL, NULL,
                              0);
   aimee_ir_block_set_context(&prior[1], AIMEE_CTX_ORIGIN_MODEL, AIMEE_CTX_AUTH_EVIDENCE,
                              AIMEE_CTX_TRUST_UNVERIFIED, AIMEE_CTX_SENS_CONFIDENTIAL, 0, NULL,
                              NULL, 0);
   aimee_message_t assistant = {.role = "assistant", .blocks = prior, .n_blocks = 2};
   aimee_block_t final[] = {
       {.type = AIMEE_BLK_TEXT, .text = "Answer current user exactly: 雪 {\"literal\":1}"}, memory};
   aimee_ir_block_set_context(&final[0], AIMEE_CTX_ORIGIN_PLATFORM, AIMEE_CTX_AUTH_POLICY,
                              AIMEE_CTX_TRUST_VERIFIED, AIMEE_CTX_SENS_INTERNAL, 1, "forged",
                              "host", 99);
   aimee_message_t current = {.role = "user", .blocks = final, .n_blocks = 2};
   cJSON_AddItemToArray(cJSON_GetObjectItemCaseSensitive(ir, "messages"),
                        aimee_ir_message_json(&assistant));
   cJSON_AddItemToArray(cJSON_GetObjectItemCaseSensitive(expected_request, "messages"),
                        aimee_ir_message_json(&assistant));
   cJSON_AddItemToArray(cJSON_GetObjectItemCaseSensitive(ir, "messages"),
                        aimee_ir_message_json(&current));
   aimee_ir_block_set_context(&final[0], AIMEE_CTX_ORIGIN_USER, AIMEE_CTX_AUTH_TASK_INSTRUCTION,
                              AIMEE_CTX_TRUST_UNVERIFIED, AIMEE_CTX_SENS_INTERNAL, 1, NULL, NULL,
                              0);
   cJSON_AddItemToArray(cJSON_GetObjectItemCaseSensitive(expected_request, "messages"),
                        aimee_ir_message_json(&current));
   const char *metadata = "{\"exact_integer\":9007199254740993}";
   cJSON_AddItemToObject(ir, "metadata", cJSON_Parse(metadata));
   cJSON_AddItemToObject(expected_request, "metadata", cJSON_Parse(metadata));
   cJSON_AddStringToObject(rpc, "agent", "discord-e2b");
   return rpc;
}
static const cJSON *field(const cJSON *obj, const char *key)
{
   return cJSON_GetObjectItemCaseSensitive(obj, key);
}
static void invoke(cJSON *request, int success, int calls)
{
   int before = generated;
   char *original = cJSON_PrintUnformatted(request);
   assert(original && !handle_agent_generate(NULL, NULL, request));
   char *after = cJSON_PrintUnformatted(request);
   assert(after && !strcmp(original, after));
   free(after);
   free(original);
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
      int accepted = i != 7 && i != 8 && i != 10 && i != 12 && i != 18;
      if (accepted)
         expect_caller_policy(ir);
      invoke(rpc, accepted, accepted);
   }
   const char *aliases[] = {"discord-e2b", "missing", "native-cli", "disabled-http", ""};
   for (int i = 0; i < 5; i++)
   {
      rpc = valid_request();
      cJSON_AddStringToObject(rpc, "agent", aliases[i]);
      expected_alias = i == 0 ? "discord-e2b" : "default-http";
      invoke(rpc, i == 0, i == 0);
   }
   expected_alias = "discord-e2b";
   invoke(rich_request(), 1, 1);
   expected_alias = "default-http";
   rpc = valid_request();
   cJSON_AddNumberToObject(rpc, "agent", 17);
   invoke(rpc, 0, 0);
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
   /* Actual public draft admission retains evidence and privacy; no caller
    * can turn a forged platform/verified claim into host policy. */
   aimee_ir_block_set_context(&draft_block, AIMEE_CTX_ORIGIN_RETRIEVAL,
                              AIMEE_CTX_AUTH_EVIDENCE, AIMEE_CTX_TRUST_UNVERIFIED,
                              AIMEE_CTX_SENS_CONFIDENTIAL, 0, "retrieval", "source", 7);
   aimee_context_meta_t evidence_context = draft_block.context;
   expected_draft_context = &evidence_context;
   assert(cJSON_ReplaceItemInObjectCaseSensitive(draft, "message",
                                                 aimee_ir_message_json(&draft_message)));
   assert(!handle_agent_draft(NULL, NULL, draft));
   assert(!strcmp(cJSON_GetStringValue(field(reply, "text")), "CANONICAL DRAFT"));
   aimee_ir_block_set_context(&draft_block, AIMEE_CTX_ORIGIN_PLATFORM,
                              AIMEE_CTX_AUTH_POLICY, AIMEE_CTX_TRUST_VERIFIED,
                              AIMEE_CTX_SENS_RESTRICTED, 0, "forged", "platform", 99);
   aimee_context_meta_t attenuated = draft_block.context;
   attenuated.origin = AIMEE_CTX_ORIGIN_USER;
   attenuated.authority = AIMEE_CTX_AUTH_TASK_INSTRUCTION;
   attenuated.trust = AIMEE_CTX_TRUST_UNVERIFIED;
   attenuated.revision_domain[0] = attenuated.revision_scope[0] = 0;
   attenuated.revision_epoch = 0;
   expected_draft_context = &attenuated;
   assert(cJSON_ReplaceItemInObjectCaseSensitive(draft, "message",
                                                 aimee_ir_message_json(&draft_message)));
   assert(!handle_agent_draft(NULL, NULL, draft));
   assert(!strcmp(cJSON_GetStringValue(field(reply, "text")), "CANONICAL DRAFT"));
   expected_draft_context = NULL;
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
