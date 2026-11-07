#define _GNU_SOURCE
#include "aimee.h"
#include "agent_protocol.h"
#include <aimee/ir/aimee_ir.h>
#include <aimee/ir/session_event.h>
#include <aimee/translation/aimee_backend.h>
#include <aimee/translation/harness_ir.h>
#include <aimee/tools/agent_tools.h>
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
extern cJSON *openai_ir_render_tool(const aimee_tool_t *, int);
#include "../server/module_native_tools.h"
int agent_tools_builtin_name_reserved(const char *name)
{
   return !strcmp(name, "builtin_tool");
}
int agent_tools_is_mcp_derived(const char *name)
{
   return !strcmp(name, "existing_mcp");
}
#define TSURF_RESP              2u
#define module_native_collision agent_tools_module_native_collision
extern int aimee_module_tool_source_mode(const char *) __attribute__((weak));
static int module_native_emit(cJSON *tools, unsigned surface)
{
   aimee_request_t snapshot = {0};
   if (agent_tools_module_native_snapshot(&snapshot))
      return -1;
   int rc = 0;
   for (int i = 0; !rc && i < snapshot.n_tools; ++i)
   {
      const aimee_tool_t *definition = snapshot.tools + i;
      if (aimee_module_tool_source_mode && aimee_module_tool_source_mode(definition->name) < 0)
         continue;
      cJSON *entry = openai_ir_render_tool(definition, surface == TSURF_RESP);
      if (!entry || !cJSON_AddItemToArray(tools, entry))
      {
         cJSON_Delete(entry);
         rc = -1;
      }
   }
   aimee_request_free(&snapshot);
   return rc;
}

static int calls, cancel_callback, null_callback;
static char *callback(const char *name, const cJSON *args, int timeout, int *error, void *state)
{
   assert(!strcmp(name, "module_sample") && timeout == 5000 && state == &calls);
   aimee_tool_t fresh = {0};
   assert(!agent_tools_module_native_definition(name, &fresh));
   aimee_tool_free(&fresh);
   calls++;
   assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(args, "ref")),
                  "9007199254740993"));
   if (cancel_callback)
   {
      pthread_cancel(pthread_self());
      pthread_testcancel();
   }
   *error = 0;
   if (null_callback)
      return NULL;
   return strdup("{\"status\":\"ok\"}");
}
static cJSON *schema(void)
{
   return cJSON_Parse(
       "{\"type\":\"object\",\"properties\":{\"ref\":{\"type\":\"string\",\"minLength\":1,"
       "\"maxLength\":64}},\"required\":[\"ref\"],\"additionalProperties\":false}");
}
static agent_module_native_definition entry(cJSON *s)
{
   return (agent_module_native_definition){
       "module_sample",
       "Owned module test",
       s,
       {AIMEE_TOOL_CLASS_WRITE, AIMEE_TOOL_POLICY_WRITE, AIMEE_TOOL_POLICY_WRITE}};
}
static void *cancel_worker(void *unused)
{
   (void)unused;
   int error = 1;
   char *out = agent_tools_module_native_call("module_sample", "{\"ref\":\"9007199254740993\"}",
                                              5000, &error);
   free(out);
   return NULL;
}

static int admissions, deliveries, deny_on_admission, sink_fail_at, sink_failure_result = -7;
static int input_admission(const char *name, const cJSON *args, void *state)
{
   assert(!strcmp(name, "module_sample") && state == &calls);
   /* Reentrant definition lookup proves no registry mutex is held. */
   aimee_tool_t fresh = {0};
   assert(!agent_tools_module_native_definition(name, &fresh));
   aimee_tool_free(&fresh);
   admissions++;
   if (deny_on_admission && admissions >= deny_on_admission)
      return -1;
   return strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(args, "ref")), "denied")
              ? 0
              : -1;
}
static int counting_sink(const aimee_session_event_t *event, void *state)
{
   (void)event;
   assert(state == &deliveries);
   deliveries++;
   return sink_fail_at == deliveries ? sink_failure_result : 0;
}
static int pairing_sink(const aimee_session_event_t *event, void *state)
{
   cJSON *history = state;
   if (event->kind == AIMEE_SESSION_MESSAGE)
   {
      cJSON *message = aimee_ir_message_json(event->message);
      assert(message && cJSON_AddItemToArray(history, message));
   }
   return 0;
}
static void *checked_worker(void *opaque)
{
   const char *value = opaque;
   cJSON *args = cJSON_CreateObject();
   assert(args && cJSON_AddStringToObject(args, "ref", value));
   aimee_block_t block = {
       .type = AIMEE_BLK_TOOL_USE, .tool_name = "module_sample", .tool_input = args};
   aimee_response_t response = {.role = "assistant", .content = &block, .n_content = 1};
   aimee_ir_session_set_sink(counting_sink, &deliveries);
   int rc = aimee_ir_session_emit_buffered_response(&response, "worker");
   assert(!strcmp(value, "denied") ? rc < 0 : rc == 0);
   aimee_ir_session_set_sink(NULL, NULL);
   cJSON_Delete(args);
   return NULL;
}
static void checked_case(int which, agent_module_native_definition *def)
{
   assert(!agent_tools_module_native_install_checked_v1(def, 1, callback, input_admission, &calls));
   aimee_tool_t tool = {0};
   assert(!agent_tools_module_native_definition("module_sample", &tool));
   assert(agent_tools_module_native_requires_buffered_v1(&tool, 1) == 1);
   assert(agent_tools_module_native_requires_buffered_v1(NULL, 0) == 0);
   aimee_tool_free(&tool);
   cJSON *args = cJSON_Parse(which == 13 ? "{\"ref\":\"denied\"}" : "{\"ref\":\"safe\"}");
   assert(args);
   aimee_block_t block = {
       .type = AIMEE_BLK_TOOL_USE, .tool_name = "module_sample", .tool_input = args};
   aimee_response_t response = {.role = "assistant", .content = &block, .n_content = 1};
   aimee_ir_session_set_sink(counting_sink, &deliveries);
   if (which == 12 || which == 13)
   {
      int rc = aimee_ir_session_emit_buffered_response(&response, "owned");
      assert((which == 12 ? rc == 0 : rc < 0));
      assert(deliveries == (which == 12 ? 3 : 0));
      assert(admissions >= 1 && !calls);
   }
   else if (which == 14)
   {
      aimee_delta_t delta = {.type = AIMEE_DELTA_BLOCK_START,
                             .kind = AIMEE_BLK_TOOL_USE,
                             .tool_name = "module_sample"};
      aimee_session_event_t event = {.kind = AIMEE_SESSION_DELTA, .delta = &delta};
      assert(aimee_ir_session_emit(&event) < 0 && !deliveries && !admissions);
      /* A producer ignoring the first refusal cannot leak later argument data. */
      event.kind = AIMEE_SESSION_MESSAGE;
      aimee_message_t message = {.role = "assistant", .blocks = &block, .n_blocks = 1};
      event.message = &message;
      assert(aimee_ir_session_emit(&event) < 0 && !deliveries && !admissions);
   }
   else if (which == 15)
   {
      block.tool_name = "ordinary_tool";
      assert(!aimee_ir_session_emit_buffered_response(&response, "ordinary"));
      assert(deliveries == 3 && !admissions);
   }
   else if (which == 16)
   {
      cJSON_AddStringToObject(args, "extra", "safe");
      assert(aimee_ir_session_emit_buffered_response(&response, "invalid") < 0 && !deliveries &&
             !admissions);
      aimee_ir_session_set_sink(NULL, NULL);
      aimee_ir_session_set_sink(counting_sink, &deliveries);
      cJSON_DeleteItemFromObjectCaseSensitive(args, "extra");
      cJSON_AddStringToObject(args, "ref", "denied");
      assert(aimee_ir_session_emit_buffered_response(&response, "duplicate") < 0 && !deliveries &&
             !admissions);
   }
   else if (which == 17)
   {
      module_native_collision("module_sample");
      assert(aimee_ir_session_emit_buffered_response(&response, "collision") < 0 && !deliveries &&
             !admissions);
   }
   else if (which == 18)
   {
      pthread_t thread;
      assert(!pthread_create(&thread, NULL, checked_worker, "safe"));
      assert(!pthread_join(thread, NULL));
      assert(deliveries == 3 && admissions >= 1);
      deliveries = 0;
      admissions = 0;
      assert(!pthread_create(&thread, NULL, checked_worker, "denied"));
      assert(!pthread_join(thread, NULL));
      assert(!deliveries && admissions == 1);
   }
   else if (which == 19)
   {
      /* Sink changes cannot remove the process-wide startup policy. */
      void *saved = NULL;
      aimee_ir_session_sink_fn sink = aimee_ir_session_get_sink(&saved);
      aimee_ir_session_set_sink(NULL, NULL);
      cJSON_ReplaceItemInObjectCaseSensitive(args, "ref", cJSON_CreateString("denied"));
      assert(aimee_ir_session_emit_buffered_response(&response, "nested") < 0);
      aimee_ir_session_restore_sink(sink, saved, aimee_ir_session_capture_failed());
      assert(aimee_ir_session_emit_buffered_response(&response, "restored") < 0 && !deliveries);
   }
   else if (which == 20)
   {
      block.tool_namespace = "unrelated";
      assert(!aimee_ir_session_emit_buffered_response(&response, "namespace"));
      assert(deliveries == 3 && !admissions);
      aimee_delta_t delta = {.type = AIMEE_DELTA_BLOCK_START,
                             .kind = AIMEE_BLK_TOOL_USE,
                             .tool_name = "module_sample",
                             .tool_namespace = "unrelated"};
      aimee_session_event_t event = {.kind = AIMEE_SESSION_DELTA, .delta = &delta};
      assert(!aimee_ir_session_emit(&event) && deliveries == 4 && !admissions);
      aimee_tool_t child = {.kind = AIMEE_TOOL_FUNCTION, .name = "module_sample"};
      aimee_tool_t namespace = {
          .kind = AIMEE_TOOL_NAMESPACE, .name = "unrelated", .children = &child, .n_children = 1};
      assert(!agent_tools_module_native_requires_buffered_v1(&namespace, 1));
   }
   else if (which == 21)
   {
      aimee_message_t message = {.role = "assistant", .n_blocks = 1};
      aimee_session_event_t event = {.kind = AIMEE_SESSION_MESSAGE, .message = &message};
      assert(aimee_ir_session_emit(&event) < 0 && !deliveries && !admissions);
      aimee_ir_session_set_sink(NULL, NULL);
      aimee_ir_session_set_sink(counting_sink, &deliveries);
      message.n_blocks = -1;
      assert(aimee_ir_session_emit(&event) < 0 && !deliveries);
      aimee_ir_session_set_sink(NULL, NULL);
      aimee_ir_session_set_sink(counting_sink, &deliveries);
      message.n_blocks = AIMEE_IR_MAX_BLOCKS + 1;
      message.blocks = &block;
      assert(aimee_ir_session_emit(&event) < 0 && !deliveries);
   }
   else if (which >= 22 && which <= 27)
   {
      int wire = (which - 22) / 2, deny = (which - 22) % 2;
      cJSON *root = NULL, *input = cJSON_CreateObject();
      assert(input && cJSON_AddStringToObject(input, "ref", deny ? "denied" : "safe"));
      if (wire == 0)
         root = cJSON_Parse("{\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":null,"
                            "\"tool_calls\":[{\"id\":\"call1\",\"type\":\"function\",\"function\":{"
                            "\"name\":\"module_sample\"}}]},\"finish_reason\":\"tool_calls\"}]}");
      if (wire == 1)
         root = cJSON_Parse(
             "{\"id\":\"resp1\",\"object\":\"response\",\"status\":\"completed\",\"output\":[{"
             "\"type\":\"function_call\",\"call_id\":\"call1\",\"name\":\"module_sample\"}]}");
      if (wire == 2)
         root = cJSON_Parse(
             "{\"id\":\"msg1\",\"type\":\"message\",\"role\":\"assistant\",\"content\":[{\"type\":"
             "\"tool_use\",\"id\":\"call1\",\"name\":\"module_sample\"}],\"stop_reason\":\"tool_"
             "use\",\"usage\":{\"input_tokens\":1,\"output_tokens\":1}}");
      assert(root);
      if (wire == 2)
         assert(cJSON_AddItemToObject(
             cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(root, "content"), 0), "input",
             input));
      else
      {
         char *raw = cJSON_PrintUnformatted(input);
         assert(raw);
         cJSON *dest =
             wire == 1 ? cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(root, "output"), 0)
                       : cJSON_GetObjectItemCaseSensitive(
                             cJSON_GetArrayItem(
                                 cJSON_GetObjectItemCaseSensitive(
                                     cJSON_GetObjectItemCaseSensitive(
                                         cJSON_GetArrayItem(
                                             cJSON_GetObjectItemCaseSensitive(root, "choices"), 0),
                                         "message"),
                                     "tool_calls"),
                                 0),
                             "function");
         assert(cJSON_AddStringToObject(dest, "arguments", raw));
         free(raw);
         cJSON_Delete(input);
      }
      aimee_response_t parsed = {0};
      char error[128] = {0};
      int rc = wire == 0   ? openai_backend_parse(root, &parsed, error, sizeof error)
               : wire == 1 ? responses_backend_parse(root, &parsed, error, sizeof error)
                           : anthropic_backend_parse(root, &parsed, error, sizeof error);
      assert(!rc && !aimee_ir_response_admit(&parsed));
      rc = aimee_ir_session_emit_buffered_response(&parsed, "actual-backend");
      assert(deny ? rc < 0 : rc == 0);
      assert(deliveries == (deny ? 0 : 3) && admissions && !calls);
      aimee_response_free(&parsed);
      /* The production decoder must distinguish admission from a sink returning
       * the SAME -1, across Chat, Responses and Anthropic, without any execution. */
      for (int sink_failure = 0; sink_failure <= 1; sink_failure++)
      {
         deliveries = admissions = 0;
         sink_fail_at = sink_failure ? 1 : 0;
         sink_failure_result = -1;
         aimee_ir_session_set_sink(NULL, NULL);
         aimee_ir_session_set_sink(counting_sink, &deliveries);
         if (wire == 1)
         {
            char *body = cJSON_PrintUnformatted(root);
            assert(body);
            rc = agent_ir_decode_responses(body, -1, NULL, &parsed);
            free(body);
         }
         else
            rc = agent_ir_decode_json_response_bound(root, wire == 2, -1, NULL, NULL, &parsed);
         assert(rc == (deny           ? AGENT_IR_INPUT_ADMISSION_FAILED
                       : sink_failure ? AGENT_IR_CAPTURE_FAILED
                                      : 0));
         assert(deliveries == (deny ? 0 : sink_failure ? 1 : 3) && admissions && !calls);
         if (rc)
            assert(aimee_ir_session_capture_failed());
         aimee_response_free(&parsed);
      }
      cJSON_Delete(root);
   }
   else if (which == 28 || which == 29)
   {
      aimee_harness_ir_t harness = {0};
      if (which == 28)
      {
         cJSON *start = cJSON_Parse(
             "{\"type\":\"stream_event\",\"event\":{\"type\":\"message_start\",\"message\":{\"id\":"
             "\"harness1\",\"type\":\"message\",\"role\":\"assistant\",\"content\":[],\"usage\":{"
             "\"input_tokens\":1,\"output_tokens\":0}}}}");
         assert(start && !aimee_claude_frame_ir(start, &harness));
         cJSON_Delete(start);
         int prior = deliveries;
         cJSON *tool = cJSON_Parse("{\"type\":\"stream_event\",\"event\":{\"type\":\"content_block_"
                                   "start\",\"index\":0,\"content_block\":{\"type\":\"tool_use\","
                                   "\"id\":\"call1\",\"name\":\"module_sample\",\"input\":{}}}}");
         /* SDK frames have no model authority: the current harness handles only
          * controls/diagnostics. The original gateway capture owns input admission. */
         assert(tool && !aimee_claude_frame_ir(tool, &harness) && deliveries == prior &&
                !admissions && !calls);
         cJSON_Delete(tool);
         assert(!aimee_ir_session_capture_failed());
      }
      else
      {
         cJSON *frame = cJSON_Parse(
             "{\"type\":\"assistant\",\"message\":{\"id\":\"harness1\",\"role\":\"assistant\","
             "\"type\":\"message\",\"content\":[{\"type\":\"tool_use\",\"id\":\"call1\",\"name\":"
             "\"module_sample\",\"input\":{\"ref\":\"denied\"}}],\"stop_reason\":\"tool_use\"}}");
         assert(frame && !aimee_claude_frame_ir(frame, &harness) && !deliveries && !admissions &&
                !calls);
         cJSON_Delete(frame);
         assert(!aimee_ir_session_capture_failed());
      }
      aimee_harness_ir_free(&harness);
   }
   else
      abort();
   aimee_ir_session_set_sink(NULL, NULL);
   cJSON_Delete(args);
}

static void diagnostic_case(int which, agent_module_native_definition *def)
{
   assert(!agent_tools_module_native_install_checked_v1(def, 1, callback, input_admission, &calls));
   if (which == 36)
   {
      cJSON *args = cJSON_Parse("{\"ref\":\"9007199254740993\"}"), *history = cJSON_CreateArray();
      assert(args && history);
      aimee_block_t block = {.type = AIMEE_BLK_TOOL_USE,
                             .tool_id = "actual-paired-call",
                             .tool_name = "module_sample",
                             .tool_input = args};
      aimee_message_t message = {.role = "assistant", .blocks = &block, .n_blocks = 1};
      aimee_request_t request = {.messages = &message, .n_messages = 1};
      aimee_ir_classify_context(&request);
      aimee_response_t response = {.role = "assistant", .content = &block, .n_content = 1};
      aimee_ir_session_set_sink(pairing_sink, history);
      assert(!aimee_ir_session_emit_buffered_response(&response, "actual-response"));
      assert(aimee_ir_history_admit(history) < 0 && calls == 0);
      int error = 1;
      char *result = agent_tools_module_native_call("module_sample",
                                                    "{\"ref\":\"9007199254740993\"}", 5000, &error);
      assert(result && !error && calls == 1);
      assert(!aimee_ir_session_emit_tool_result("actual-paired-call", result, 0));
      assert(cJSON_GetArraySize(history) == 2 && !aimee_ir_history_admit(history));
      free(result);
      aimee_ir_session_set_sink(NULL, NULL);
      cJSON_Delete(history);
      cJSON_Delete(args);
      return;
   }
   int enabled = which != 30;
   assert(!setenv("AIMEE_IR_CAPTURE_PHASE_DIAGNOSTIC", enabled ? "1" : "0", 1));
   FILE *file = tmpfile();
   assert(file && fflush(stderr) == 0);
   int saved = dup(STDERR_FILENO);
   assert(saved >= 0 && dup2(fileno(file), STDERR_FILENO) >= 0);
   cJSON *args =
       cJSON_Parse(which == 30 || which == 31 ? "{\"ref\":\"denied\"}"
                                              : "{\"ref\":\"private-argument-sentinel\"}");
   assert(args);
   aimee_block_t block = {.type = AIMEE_BLK_TOOL_USE,
                          .tool_id = "private-call-id-sentinel",
                          .tool_name = "module_sample",
                          .tool_input = args};
   aimee_response_t response = {.role = "assistant", .content = &block, .n_content = 1};
   aimee_ir_session_set_sink(counting_sink, &deliveries);
   if (which == 33)
   {
      aimee_delta_t delta = {.type = AIMEE_DELTA_BLOCK_START,
                             .kind = AIMEE_BLK_TOOL_USE,
                             .tool_name = "module_sample"};
      aimee_session_event_t event = {.kind = AIMEE_SESSION_DELTA, .delta = &delta};
      assert(aimee_ir_session_emit(&event) < 0);
      aimee_message_t message = {.role = "assistant", .blocks = &block, .n_blocks = 1};
      event.kind = AIMEE_SESSION_MESSAGE;
      event.message = &message;
      assert(aimee_ir_session_emit(&event) < 0 && !deliveries && !admissions && !calls);
      aimee_ir_capture_cause_t cause = AIMEE_IR_CAPTURE_CAUSE_NONE;
      assert(aimee_ir_session_emit_buffered_response_checked(&response, "after-failure", &cause) <
             0);
      assert(cause == AIMEE_IR_CAPTURE_CAUSE_PREVIOUS_FAILURE && !deliveries && admissions == 1 &&
             !calls);
   }
   else
   {
      if (which == 34)
         deny_on_admission = 2;
      if (which == 35)
         sink_fail_at = 2;
      aimee_ir_capture_cause_t cause = AIMEE_IR_CAPTURE_CAUSE_SINK;
      int rc = aimee_ir_session_emit_buffered_response_checked(
          &response, "private-message-id-sentinel", &cause);
      if (which == 32)
         assert(!rc && deliveries == 3 && admissions == 2);
      else
      {
         assert(rc == (which == 35 ? -7 : -1) && aimee_ir_session_capture_failed() && !calls);
         assert(deliveries == ((which == 34) ? 1 : (which == 35) ? 2 : 0));
      }
      assert(cause == (which == 32   ? AIMEE_IR_CAPTURE_CAUSE_NONE
                       : which == 35 ? AIMEE_IR_CAPTURE_CAUSE_SINK
                                     : AIMEE_IR_CAPTURE_CAUSE_INPUT_ADMISSION));
   }
   assert(fflush(stderr) == 0 && dup2(saved, STDERR_FILENO) >= 0);
   close(saved);
   rewind(file);
   char output[4096];
   size_t n = fread(output, 1, sizeof output - 1, file);
   assert(!ferror(file) && n < sizeof output - 1);
   output[n] = 0;
   fclose(file);
   assert(!strstr(output, "private-argument-sentinel") &&
          !strstr(output, "private-call-id-sentinel") &&
          !strstr(output, "private-message-id-sentinel") && !strstr(output, "module_sample"));
   if (which == 30 || which == 32)
      assert(!output[0]);
   if (which == 31)
      assert(strstr(output, "buffered_tool_admission") && !strstr(output, "session_sink"));
   if (which == 33)
      assert(strstr(output, "capture_already_failed") && !strstr(output, "session_sink"));
   if (which == 34)
      assert(strstr(output, "event_tool_admission") && strstr(output, "buffered_message") &&
             !strstr(output, "session_sink"));
   if (which == 35)
      assert(strstr(output, "session_sink") && strstr(output, "buffered_message"));
   aimee_ir_session_set_sink(NULL, NULL);
   cJSON_Delete(args);
}
static void run_case(int which)
{
   cJSON *s = schema();
   assert(s);
   agent_module_native_definition def = entry(s);
   if (which >= 30)
   {
      diagnostic_case(which, &def);
      goto done;
   }
   if (which >= 12)
   {
      checked_case(which, &def);
      goto done;
   }
   if (which == 0)
   {
      cJSON *catalog = cJSON_CreateArray();
      assert(!module_native_emit(catalog, 0) && !cJSON_GetArraySize(catalog));
      cJSON_Delete(catalog);
      assert(agent_tools_module_native_install(&def, 1, callback, &calls) < 0);
      goto done;
   }
   if (which == 1)
   {
      def.name = "builtin_tool";
      assert(agent_tools_module_native_install(&def, 1, callback, &calls) < 0);
      def.name = "existing_mcp";
      assert(agent_tools_module_native_install(&def, 1, callback, &calls) < 0);
      def.name = "bad:name";
      assert(agent_tools_module_native_install(&def, 1, callback, &calls) < 0);
      goto done;
   }
   if (which == 2)
   {
      def.metadata = (aimee_tool_metadata_t){0};
      assert(agent_tools_module_native_install(&def, 1, callback, &calls) < 0);
      def = entry(s);
      agent_module_native_definition pair[2] = {def, def};
      assert(agent_tools_module_native_install(pair, 2, callback, &calls) < 0);
      cJSON *ref = cJSON_GetObjectItemCaseSensitive(
          cJSON_GetObjectItemCaseSensitive(s, "properties"), "ref");
      cJSON_AddStringToObject(ref, "pattern", ".*");
      assert(agent_tools_module_native_install(&def, 1, callback, &calls) < 0);
      cJSON_DeleteItemFromObjectCaseSensitive(ref, "pattern");
      cJSON_ReplaceItemInObjectCaseSensitive(ref, "type", cJSON_CreateString("object"));
      assert(agent_tools_module_native_install(&def, 1, callback, &calls) < 0);
      cJSON_ReplaceItemInObjectCaseSensitive(ref, "type", cJSON_CreateString("number"));
      assert(agent_tools_module_native_install(&def, 1, callback, &calls) < 0);
      cJSON_ReplaceItemInObjectCaseSensitive(ref, "type", cJSON_CreateString("integer"));
      assert(agent_tools_module_native_install(&def, 1, callback, &calls) < 0);
      cJSON_ReplaceItemInObjectCaseSensitive(ref, "type", cJSON_CreateString("string"));
      cJSON_AddBoolToObject(s, "description", 1);
      assert(agent_tools_module_native_install(&def, 1, callback, &calls) < 0);
      cJSON_DeleteItemFromObjectCaseSensitive(s, "description");
      cJSON_AddBoolToObject(ref, "description", 1);
      assert(agent_tools_module_native_install(&def, 1, callback, &calls) < 0);
      cJSON_DeleteItemFromObjectCaseSensitive(ref, "description");
      goto done;
   }
   assert(!agent_tools_module_native_install(&def, 1, callback, &calls));
   assert(agent_tools_module_native_install(&def, 1, callback, &calls) < 0);
   cJSON_Delete(s);
   s = NULL;
   if (which == 3)
   {
      cJSON *chat = cJSON_CreateArray(), *resp = cJSON_CreateArray();
      assert(!module_native_emit(chat, 0) && !module_native_emit(resp, TSURF_RESP));
      const cJSON *a = cJSON_GetArrayItem(chat, 0),
                  *f = cJSON_GetObjectItemCaseSensitive(a, "function"),
                  *r = cJSON_GetArrayItem(resp, 0);
      assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(f, "name")),
                     "module_sample"));
      assert(cJSON_Compare(cJSON_GetObjectItemCaseSensitive(f, "parameters"),
                           cJSON_GetObjectItemCaseSensitive(r, "parameters"), 1));
      cJSON_Delete(chat);
      cJSON_Delete(resp);
      goto done;
   }
   if (which == 4)
   {
      aimee_tool_t d = {0};
      assert(!agent_tools_module_native_definition("module_sample", &d));
      assert(agent_tools_module_native_admitted(&d));
      cJSON_AddStringToObject(d.schema, "fake", "x");
      assert(!agent_tools_module_native_admitted(&d));
      aimee_tool_free(&d);
      goto done;
   }
   if (which == 5)
   {
      const char *bad[] = {"{}",
                           "{\"ref\":5}",
                           "{\"ref\":9007199254740993}",
                           "{\"ref\":\"x\",\"extra\":1}",
                           "{\"ref\":\"a\",\"ref\":\"b\"}",
                           "{\"ref\":\"a\\u0000b\"}",
                           "{\"ref\":\"x\"}{}"};
      for (size_t i = 0; i < sizeof bad / sizeof *bad; i++)
      {
         int error = 0;
         char *out = agent_tools_module_native_call("module_sample", bad[i], 5000, &error);
         assert(error && out && !calls);
         free(out);
      }
      goto done;
   }
   if (which == 6)
   {
      int error = 1;
      char *out = agent_tools_module_native_call("module_sample", "{\"ref\":\"9007199254740993\"}",
                                                 5000, &error);
      assert(!error && out && calls == 1);
      free(out);
      goto done;
   }
   if (which == 7)
   {
      cancel_callback = 1;
      pthread_t t;
      assert(!pthread_create(&t, NULL, cancel_worker, NULL));
      void *out = NULL;
      assert(!pthread_join(t, &out) && out == PTHREAD_CANCELED && calls == 1);
      goto done;
   }
   if (which == 8)
   {
      module_native_collision("module_sample");
      cJSON *catalog = cJSON_CreateArray();
      assert(module_native_emit(catalog, 0) < 0);
      cJSON_Delete(catalog);
      assert(agent_tools_module_native_has("module_sample"));
      int error = 0;
      char *out = agent_tools_module_native_call("module_sample", "{\"ref\":\"9007199254740993\"}",
                                                 5000, &error);
      assert(error && out && !calls);
      free(out);
      goto done;
   }
   if (which == 9)
   {
      aimee_tool_metadata_t metadata;
      assert(!agent_tools_module_native_metadata("module_sample", &metadata));
      assert(metadata.classification == AIMEE_TOOL_CLASS_WRITE &&
             metadata.primary == AIMEE_TOOL_POLICY_WRITE);
      assert(agent_tools_module_native_metadata("unknown", &metadata) < 0);
      goto done;
   }
   if (which == 10)
   {
      null_callback = 1;
      int error = 0;
      char *out = agent_tools_module_native_call("module_sample", "{\"ref\":\"9007199254740993\"}",
                                                 5000, &error);
      assert(error == 1 && out && strstr(out, "outcome unavailable") && calls == 1);
      free(out);
      goto done;
   }
   if (which == 11)
   {
      int prior = calls;
      char *out = agent_tools_module_native_call("module_sample", "{\"ref\":\"9007199254740993\"}",
                                                 5000, NULL);
      assert(out && calls == prior + 1);
      free(out);
      goto done;
   }
   abort();
done:
   cJSON_Delete(s);
   printf("PASS registry case%d\n", which);
}
int main(void)
{
#ifdef EXPECT_MISSING_IR_ADMISSION
   cJSON *s = schema();
   agent_module_native_definition def = entry(s);
   assert(agent_tools_module_native_install_checked_v1(&def, 1, callback, input_admission, &calls) <
          0);
   assert(!agent_tools_module_native_install(&def, 1, callback, &calls));
   cJSON_Delete(s);
   puts("PASS old IR cannot install checked adapter; legacy installer remains available");
   return 0;
#endif
   for (int i = 0; i < 37; i++)
   {
      pid_t p = fork();
      assert(p >= 0);
      if (!p)
      {
         alarm(10);
         run_case(i);
         fflush(stdout);
         _exit(0);
      }
      int status;
      assert(waitpid(p, &status, 0) == p && WIFEXITED(status) && !WEXITSTATUS(status));
   }
   return 0;
}
