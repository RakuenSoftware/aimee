#include "gw_stage_completion.h"
#include "cJSON.h"
#include <aimee/ir/session_event.h>

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static cJSON *json(const char *text)
{
   cJSON *out = cJSON_Parse(text);
   assert(out);
   return out;
}

static aimee_request_t request(const char *messages, const char *tool, const char *schema)
{
   aimee_request_t out = {0};
   cJSON *history = json(messages);
   out.n_messages = cJSON_GetArraySize(history);
   out.messages = calloc((size_t)out.n_messages, sizeof *out.messages);
   assert(out.messages);
   for (int i = 0; i < out.n_messages; i++)
      assert(!aimee_ir_message_parse(cJSON_GetArrayItem(history, i), &out.messages[i]));
   cJSON_Delete(history);
   if (tool)
   {
      out.n_tools = 1;
      out.tools = calloc(1, sizeof *out.tools);
      assert(out.tools);
      out.tools[0].name = strdup(tool);
      out.tools[0].schema = json(schema);
      assert(out.tools[0].name);
   }
   return out;
}

static aimee_response_t response(const char *text)
{
   aimee_response_t out = {0};
   out.n_content = 2;
   out.content = calloc(2, sizeof *out.content);
   assert(out.content);
   out.content[0] = (aimee_block_t){.type = AIMEE_BLK_THINKING,
                                    .text = strdup("private"),
                                    .thinking_signature = strdup("signed")};
   out.content[1] = (aimee_block_t){.type = AIMEE_BLK_TEXT, .text = strdup(text)};
   out.stop_reason = AIMEE_STOP_END_TURN;
   return out;
}

static const char edit_history[] =
    "[{\"role\":\"user\",\"blocks\":[{\"type\":\"text\",\"text\":\"repair the unsafe path\"}]},"
    "{\"role\":\"assistant\",\"blocks\":[{\"type\":\"tool_use\",\"tool_id\":\"edit\","
    "\"name\":\"apply_patch\",\"input\":{}}]}]";
static const char cmd_schema[] =
    "{\"type\":\"object\",\"properties\":{\"cmd\":{\"type\":\"string\"}}}";
static const char empty_schema[] = "{\"type\":\"object\"}";

static void test_intervention_retains_reasoning_and_bounds_retry(void)
{
   const char *texts[] = {
       "Heads-up: the same unsafe path pattern exists in two report handlers. Want me to apply the "
       "same containment check there?",
       "Related findings were not changed.",
       "One heads-up: app/reports.py has the same flagged traversal pattern but was outside the "
       "scope of this request - want me to apply the same fix there?"};
   for (int i = 0; i < 3; i++)
      for (int bash = 0; bash <= 1; bash++)
      {
         aimee_request_t policy = request(edit_history, bash ? "Bash" : "exec_command",
                                          bash ? empty_schema : cmd_schema);
         aimee_response_t out = response(texts[i]);
         assert(gw_ir_response_completion_armed(&policy));
         assert(gw_ir_response_run_completion(&out, &policy) == 1);
         assert(out.n_content == 3 && !strcmp(out.content[0].thinking_signature, "signed"));
         assert(!strcmp(out.content[1].text, texts[i]));
         const aimee_block_t *call = &out.content[2];
         assert(call->type == AIMEE_BLK_TOOL_USE && !strcmp(call->tool_name, policy.tools[0].name));
         assert(!strncmp(call->tool_id, AIMEE_COMPLETION_CALL_PREFIX,
                         strlen(AIMEE_COMPLETION_CALL_PREFIX)));
         assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(
                            call->tool_input, bash ? "command" : "cmd")),
                        "true"));
         assert(call->context.origin == AIMEE_CTX_ORIGIN_PLATFORM &&
                call->context.authority == AIMEE_CTX_AUTH_POLICY);
         assert(out.stop_reason == AIMEE_STOP_TOOL_USE);
         assert(!gw_ir_response_run_completion(&out, &policy) && out.n_content == 3);
         /* Feed the actual canonical intervention into the next request. Its
          * marker prevents another retry and supplies the continuation policy. */
         aimee_message_t next = {
             .role = strdup("assistant"), .blocks = out.content, .n_blocks = out.n_content};
         policy.messages =
             realloc(policy.messages, (size_t)(policy.n_messages + 1) * sizeof *policy.messages);
         assert(policy.messages);
         policy.messages[policy.n_messages++] = next;
         out.content = NULL;
         out.n_content = 0;
         assert(!gw_ir_response_completion_armed(&policy));
         char *prompt = gw_ir_request_completion_system_prompt(&policy, "existing policy");
         assert(prompt && strstr(prompt, "existing policy") &&
                strstr(prompt, "<aimee-completion-policy>"));
         assert(strstr(prompt, "unless the user explicitly excluded it"));
         free(prompt);
         aimee_response_free(&out);
         aimee_request_free(&policy);
      }
}

static void test_no_unrelated_or_evidence_intervention(void)
{
   const char *histories[] = {
       "[{\"role\":\"user\",\"blocks\":[{\"type\":\"text\",\"text\":\"apply_patch cat > file\"}]}]",
       "[{\"role\":\"tool\",\"blocks\":[{\"type\":\"tool_result\",\"is_error\":false,\"tool_id\":"
       "\"edit\",\"content\":"
       "\"apply_patch cat > file\"}]}]",
       "[{\"role\":\"assistant\",\"blocks\":[{\"type\":\"text\",\"text\":\"apply_patch cat > "
       "file\"}]}]"};
   for (int i = 0; i < 3; i++)
   {
      aimee_request_t policy = request(histories[i], "Bash", empty_schema);
      aimee_response_t out = response("Related findings were not changed.");
      assert(!gw_ir_response_completion_armed(&policy));
      assert(!gw_ir_response_run_completion(&out, &policy) && out.n_content == 2);
      assert(!gw_ir_request_completion_system_prompt(&policy, "base"));
      aimee_response_free(&out);
      aimee_request_free(&policy);
   }
   aimee_request_t policy = request(edit_history, "Bash", empty_schema);
   aimee_response_t out = response("I fixed the requested file. Want me to update the docs too?");
   assert(!gw_ir_response_run_completion(&out, &policy));
   aimee_response_free(&out);
   policy.tools[0].name[0] = 'x';
   out = response("Related findings were not changed.");
   assert(!gw_ir_response_completion_armed(&policy));
   assert(!gw_ir_response_run_completion(&out, &policy));
   assert(!gw_ir_request_tool_system_prompt(&policy, "base"));
   aimee_response_free(&out);
   aimee_request_free(&policy);
}

static void test_shell_edit_and_instruction_surface(void)
{
   aimee_request_t policy =
       request("[{\"role\":\"assistant\",\"blocks\":[{\"type\":\"tool_use\",\"tool_id\":\"edit\","
               "\"name\":\"exec_command\",\"input\":{\"cmd\":\"cat > app/files.py <<EOF\"}}]}]",
               "exec_command", cmd_schema);
   assert(gw_ir_response_completion_armed(&policy));
   char *prompt = gw_ir_request_tool_system_prompt(&policy, "existing policy");
   assert(prompt && strstr(prompt, "existing policy") && strstr(prompt, "<aimee-tool-policy>"));
   assert(strstr(prompt, "CLI is the preferred registered") &&
          strstr(prompt, "MCP is the alternative"));
   assert(strstr(prompt, "root cause, not merely the named") &&
          strstr(prompt, "aimee index ast-grep"));
   assert(strstr(prompt, "preserving the existing success-path contract"));
   free(prompt);
   aimee_request_free(&policy);
}

static void test_provider_refusal_cannot_trigger_completion(void)
{
   for (int details = 1; details >= 0; details--)
   {
      aimee_request_t policy = request(edit_history, "Bash", empty_schema);
      aimee_response_t out = response("The same issue was left unchanged.");
      out.stop_reason = AIMEE_STOP_CONTENT_FILTER;
      out.raw_stop_reason = strdup("refusal");
      assert(out.raw_stop_reason && gw_ir_response_completion_armed(&policy));
      if (details)
      {
         out.refusal = calloc(1, sizeof *out.refusal);
         assert(out.refusal);
         out.refusal->category = strdup("future_category");
         out.refusal->explanation = strdup("Diagnostic only");
         assert(out.refusal->category && out.refusal->explanation);
      }
      assert(!aimee_ir_response_admit(&out));
      aimee_block_t *content = out.content;
      aimee_refusal_details_t *refusal = out.refusal;
      char *raw = out.raw_stop_reason;
      assert(!gw_ir_response_run_completion(&out, &policy));
      assert(out.n_content == 2 && out.content == content && !aimee_ir_response_has_tool_use(&out));
      assert(out.stop_reason == AIMEE_STOP_CONTENT_FILTER && out.raw_stop_reason == raw);
      assert(!strcmp(out.raw_stop_reason, "refusal"));
      assert(!strcmp(out.content[1].text, "The same issue was left unchanged."));
      assert(!strcmp(out.content[0].thinking_signature, "signed"));
      assert(out.refusal == refusal && !aimee_ir_response_admit(&out));
      if (details)
         assert(!strcmp(out.refusal->category, "future_category") &&
                !strcmp(out.refusal->explanation, "Diagnostic only"));
      aimee_response_free(&out);
      aimee_request_free(&policy);
   }
}

int main(void)
{
   test_provider_refusal_cannot_trigger_completion();
   test_intervention_retains_reasoning_and_bounds_retry();
   test_no_unrelated_or_evidence_intervention();
   test_shell_edit_and_instruction_surface();
   puts("response completion stage: ok");
   return 0;
}
