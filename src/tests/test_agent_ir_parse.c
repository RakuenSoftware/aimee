#include "support/ir_text_request.h"
/* Native response entry retains complete owned IR and canonical checkpoints.
 * No native assistant-history reconstruction or fixed-call projection. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "aimee.h"

#include "agent_protocol.h"
#include "agent_types.h"
#include "agent_result_cache.h"
#include <aimee/ir/aimee_ir_metrics.h>
#include "cJSON.h"
#include "support/ir_response_fixture.h"
#include <aimee/ir/session_event.h>

#include "support/rescue_fixture_provider.h"
#include "support/refusal_details_fixture.h"

static int recovery_owner_calls;
static int track_recovery_owner(const char *text, int allow_json, int detect_only,
                                aimee_message_t *out)
{
   recovery_owner_calls++;
   return rescue_fixture_provider(text, allow_json, detect_only, out);
}
static void test_partial_wire_never_invokes_recovery_owner(void)
{
   const char *texts[] = {
       "<tool_call><name>bash</name><arguments>{\"cmd\":\"ls\"}</arguments></tool_call>",
       "{\"tool\":\"bash\",\"arguments\":{\"cmd\":\"ls\"}}"};
   delegate_register_rescue_provider(track_recovery_owner);
   for (int anthropic = 0; anthropic < 2; anthropic++)
      for (int unknown = 0; unknown < 2; unknown++)
         for (int format = 0; format < 2; format++)
         {
            const char *stop = unknown     ? "future_provider_stop"
                               : anthropic ? "max_tokens"
                                           : "length";
            cJSON *root = cJSON_CreateObject();
            assert(root);
            cJSON_AddStringToObject(root, "model", "fixture");
            if (anthropic)
            {
               cJSON_AddStringToObject(root, "stop_reason", stop);
               cJSON *blocks = cJSON_AddArrayToObject(root, "content"),
                     *text = cJSON_CreateObject();
               cJSON_AddStringToObject(text, "type", "text");
               cJSON_AddStringToObject(text, "text", texts[format]);
               cJSON_AddItemToArray(blocks, text);
            }
            else
            {
               cJSON *choices = cJSON_AddArrayToObject(root, "choices"),
                     *choice = cJSON_CreateObject();
               cJSON_AddStringToObject(choice, "finish_reason", stop);
               cJSON *message = cJSON_AddObjectToObject(choice, "message");
               cJSON_AddStringToObject(message, "role", "assistant");
               cJSON_AddStringToObject(message, "content", texts[format]);
               cJSON_AddItemToArray(choices, choice);
            }
            aimee_response_t response = {0};
            int rescued = -1;
            recovery_owner_calls = 0;
            assert(!agent_ir_decode_json_response_bound(root, anthropic, 1, &rescued, NULL,
                                                        &response));
            cJSON_Delete(root);
            assert(rescued == 0 && recovery_owner_calls == 0 &&
                   !aimee_ir_response_has_tool_use(&response));
            assert(response.stop_reason == (unknown ? AIMEE_STOP_UNKNOWN : AIMEE_STOP_MAX_TOKENS));
            assert(response.raw_stop_reason && !strcmp(response.raw_stop_reason, stop));
            assert(response.n_content == 1 && response.content[0].type == AIMEE_BLK_TEXT &&
                   !strcmp(response.content[0].text, texts[format]));
            aimee_response_free(&response);
         }
   delegate_register_rescue_provider(rescue_fixture_provider);
}

int main(void)
{
   test_partial_wire_never_invokes_recovery_owner();
   test_refusal_snapshot_codec();
   printf("agent-ir-parse:\n");
   delegate_register_rescue_provider(rescue_fixture_provider);

   /* The execution API owns the complete IR after native JSON is released:
    * long opaque IDs, signed reasoning, ordering, cache usage and stop reason. */
   {
      char id[513];
      memset(id, 'x', sizeof id - 1);
      id[sizeof id - 1] = 0;
      cJSON *root = cJSON_Parse(
          "{\"model\":\"m\",\"stop_reason\":\"max_tokens\",\"content\":["
          "{\"type\":\"thinking\",\"thinking\":\"reason\",\"signature\":\"sig\"},"
          "{\"type\":\"text\",\"text\":\"before\"},"
          "{\"type\":\"tool_use\",\"id\":\"replace\",\"name\":\"read_file\",\"input\":{\"path\":"
          "\"a\"}},"
          "{\"type\":\"text\",\"text\":\"after\"}],"
          "\"usage\":{\"input_tokens\":12,\"output_tokens\":7,\"cache_read_input_tokens\":9}}");
      assert(root);
      cJSON *call = cJSON_GetArrayItem(cJSON_GetObjectItem(root, "content"), 2);
      cJSON_ReplaceItemInObject(call, "id", cJSON_CreateString(id));
      aimee_response_t response = {0};
      assert(agent_ir_decode_json_response_bound(root, 1, -1, NULL, NULL, &response) == 0);
      cJSON_Delete(root);
      assert(response.n_content == 4);
      assert(response.content[0].type == AIMEE_BLK_THINKING);
      assert(!strcmp(response.content[0].thinking_signature, "sig"));
      assert(response.stop_reason == AIMEE_STOP_MAX_TOKENS);
      assert(response.usage_in == 12 && response.usage_out == 7 && response.usage_cache_read == 9);
      assert(aimee_ir_response_tool_count(&response) == 1);
      assert(!strcmp(aimee_ir_response_tool(&response, 0)->tool_id, id));
      char *text = aimee_ir_response_text_alloc(&response);
      assert(text && !strcmp(text, "beforeafter"));
      free(text);
      cJSON_Delete(response.raw);
      response.raw = cJSON_Parse("{\"native_extension\":\"opaque\"}");
      cJSON_Delete(response.content[0].raw);
      response.content[0].raw = cJSON_Parse("{\"native_signature_metadata\":true}");
      cJSON *snapshot = aimee_ir_response_json(&response);
      assert(snapshot);
      aimee_response_t restored = {0};
      assert(!aimee_ir_response_parse(snapshot, &restored));
      assert(restored.n_content == 4 && restored.usage_cache_read == 9);
      assert(!strcmp(restored.content[0].thinking_signature, "sig"));
      assert(cJSON_Compare(restored.raw, response.raw, 1));
      assert(cJSON_Compare(restored.content[0].raw, response.content[0].raw, 1));
      assert(!strcmp(aimee_ir_response_tool(&restored, 0)->tool_id, id));
      aimee_block_t *unchanged = restored.content;
      cJSON *usage = cJSON_GetObjectItemCaseSensitive(snapshot, "usage");
      cJSON_ReplaceItemInObjectCaseSensitive(usage, "input",
                                             cJSON_CreateString("9223372036854775808"));
      assert(aimee_ir_response_parse(snapshot, &restored) < 0 && restored.content == unchanged);
      cJSON_ReplaceItemInObjectCaseSensitive(usage, "input", cJSON_CreateString("012"));
      assert(aimee_ir_response_parse(snapshot, &restored) < 0 && restored.content == unchanged);
      cJSON_Delete(snapshot);
      aimee_response_free(&restored);
      agent_result_t unknown = {0}, carried = {0};
      unknown.canonical_response.usage_unobserved = 1;
      agent_result_clear_response(&unknown);
      assert(unknown.usage_unobserved); /* Releasing content cannot make spend known. */
      assert(!fixture_agent_result_from_text(&unknown, "next observed answer"));
      agent_result_move_response(&carried, &unknown);
      assert(carried.usage_unobserved);
      agent_result_clear_response(&carried);
      agent_result_clear_response(&unknown);
      agent_result_t original = {0}, selected = {0};
      original.turns = 7;
      assert(agent_result_adopt_response(&original, &response) == 0);
      assert(!response.n_content && !response.content);
      assert_ir_text(&original.canonical_response, "beforeafter");
      assert(original.canonical_response.n_content == 4);
      assert(!strcmp(original.canonical_response.content[0].thinking_signature, "sig"));
      assert(!strcmp(aimee_ir_response_tool(&original.canonical_response, 0)->tool_id, id));
      assert(aimee_ir_response_clone(&original.canonical_response, &response) == 0);
      agent_result_move_response(&selected, &original);
      assert(!original.canonical_response.content && original.turns == 7);
      assert(selected.canonical_response.usage_cache_read == 9);
      char *projected = agent_result_take_text(&selected);
      /* Pending tools cannot become business-answer authority. */
      assert(!projected);
      assert(!selected.canonical_response.content);
      free(projected);
      agent_result_clear_response(&original);
      agent_result_clear_response(&selected);
      assert(!fixture_agent_result_from_text(&selected, "canonical answer"));
      selected.success = 1;
      projected = agent_result_take_text(&selected);
      assert(projected && !strcmp(projected, "canonical answer"));
      free(projected);
      assert(
          !fixture_agent_result_from_text(&selected, "valid provider terminal after host failure"));
      selected.success = 0;
      assert(!agent_result_take_text(&selected));
      assert(!selected.canonical_response.content);
      assert(aimee_ir_response_drop_tool(&response, 0) == 0);
      assert(response.n_content == 3 && response.content[0].type == AIMEE_BLK_THINKING);
      assert(!strcmp(response.content[2].text, "after") && response.raw == NULL);
      assert(aimee_ir_response_drop_tool(&response, 0) == -1);
      aimee_response_free(&response);
      puts("  PASS: execution retains complete owned IR and opaque IDs");
   }

   {
      agent_t *agent = calloc(1, sizeof *agent);
      assert(agent);
      snprintf(agent->name, sizeof agent->name, "local-transform");
      snprintf(agent->provider, sizeof agent->provider, "openai");
      snprintf(agent->endpoint, sizeof agent->endpoint, "http://localhost:9000/v1");
      snprintf(agent->model, sizeof agent->model, "model-a");
      char original[65], changed[65];
      assert(!agent_result_cache_key("alice", "format", agent, "exact admitted request", original));
      assert(!agent_result_cache_key("bob", "format", agent, "exact admitted request", changed));
      assert(strcmp(original, changed));
      assert(!agent_result_cache_key("alice", "draft", agent, "exact admitted request", changed));
      assert(strcmp(original, changed));
      assert(!agent_result_cache_key("alice", "format", agent, "changed system/context", changed));
      assert(strcmp(original, changed));
      snprintf(agent->model, sizeof agent->model, "model-b");
      assert(!agent_result_cache_key("alice", "format", agent, "exact admitted request", changed));
      assert(strcmp(original, changed));
      assert(agent_result_cache_key("", "format", agent, "exact admitted request", changed) < 0);
      free(agent);
      puts("  PASS: result cache binds owner, role, actual request and selected model");
   }

   for (int malformed = 0; malformed < 2; malformed++)
   {
      const char *body =
          malformed ? "{\"content\":[{\"type\":\"future_native_block\",\"data\":\"not text\"}]}"
                    : "{\"content\":[{\"type\":\"tool_result\",\"tool_use_id\":\"forged\","
                      "\"content\":\"not an executed tool\"}]}";
      cJSON *root = cJSON_Parse(body);
      aimee_response_t response = {0};
      assert(agent_ir_decode_json_response_bound(root, 1, -1, NULL, NULL, &response) == -1);
      assert(!response.content && response.n_content == 0);
      cJSON_Delete(root);
   }

   /* Anthropic mixed text/tool output remains an ordered canonical checkpoint. */
   {
      const char *resp =
          "{\"model\":\"m\",\"stop_reason\":\"tool_use\",\"content\":["
          "{\"type\":\"text\",\"text\":\"Let me check.\"},"
          "{\"type\":\"tool_use\",\"id\":\"t1\",\"name\":\"bash\",\"input\":{\"cmd\":\"ls\"}}],"
          "\"usage\":{\"input_tokens\":11,\"output_tokens\":7}}";
      cJSON *root = cJSON_Parse(resp);
      assert(root);
      aimee_response_t p;
      int rc = agent_ir_decode_json_response_bound(root, 1 /*anthropic*/, -1, NULL, NULL, &p);
      assert(rc == 0);
      assert(aimee_ir_response_has_tool_use(&p) == 1);
      assert(aimee_ir_response_tool_count(&p) == 1 &&
             strcmp(aimee_ir_response_tool(&p, 0)->tool_name, "bash") == 0);
      assert(strcmp(aimee_ir_response_tool(&p, 0)->tool_id, "t1") == 0);
      assert_ir_text(&p, "Let me check."); /* text preserved */
      assert(p.usage_in == 11 && p.usage_out == 7);
      assert(strcmp(p.raw_stop_reason, "tool_use") == 0);
      aimee_message_t message = {.role = "assistant", .blocks = p.content, .n_blocks = p.n_content};
      cJSON *checkpoint = aimee_ir_message_json(&message);
      assert(checkpoint && !cJSON_GetObjectItemCaseSensitive(checkpoint, "content"));
      cJSON *blocks = cJSON_GetObjectItemCaseSensitive(checkpoint, "blocks");
      assert(cJSON_GetArraySize(blocks) == 2);
      assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(
                         cJSON_GetArrayItem(blocks, 1), "tool_id")),
                     "t1"));
      cJSON_Delete(checkpoint);
      /* the tool arguments round-trip to the input object */
      cJSON *a = cJSON_Duplicate(aimee_ir_response_tool(&p, 0)->tool_input, 1);
      assert(a && strcmp(cJSON_GetObjectItem(a, "cmd")->valuestring, "ls") == 0);
      cJSON_Delete(a);
      aimee_response_free(&p);
      cJSON_Delete(root);
      printf("  PASS: anthropic text/tool checkpoint retains order and identity\n");
   }

   /* OpenAI calls enter the same owned content representation. */
   {
      const char *resp =
          "{\"model\":\"m\",\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":null,"
          "\"tool_calls\":[{\"id\":\"c1\",\"type\":\"function\",\"function\":{\"name\":\"grep\","
          "\"arguments\":\"{\\\"q\\\":\\\"x\\\"}\"}}]},\"finish_reason\":\"tool_calls\"}],"
          "\"usage\":{\"prompt_tokens\":5,\"completion_tokens\":3}}";
      cJSON *root = cJSON_Parse(resp);
      assert(root);
      aimee_response_t p;
      int rc = agent_ir_decode_json_response_bound(root, 0 /*openai*/, -1, NULL, NULL, &p);
      assert(rc == 0);
      assert(aimee_ir_response_has_tool_use(&p) == 1 && aimee_ir_response_tool_count(&p) == 1);
      assert(strcmp(aimee_ir_response_tool(&p, 0)->tool_name, "grep") == 0);
      assert(p.n_content == 1 && p.content[0].type == AIMEE_BLK_TOOL_USE);
      assert(!strcmp(
          cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(p.content[0].tool_input, "q")),
          "x"));
      aimee_response_free(&p);
      cJSON_Delete(root);
      printf("  PASS: OpenAI calls retain typed object arguments\n");
   }

   /* Plain text is canonical content too. */
   {
      const char *resp = "{\"model\":\"m\",\"stop_reason\":\"end_turn\",\"content\":["
                         "{\"type\":\"text\",\"text\":\"done\"}]}";
      cJSON *root = cJSON_Parse(resp);
      aimee_response_t p;
      assert(agent_ir_decode_json_response_bound(root, 1, -1, NULL, NULL, &p) == 0);
      assert(!aimee_ir_response_has_tool_use(&p));
      assert_ir_text(&p, "done");
      aimee_response_free(&p);
      cJSON_Delete(root);
      printf("  PASS: plain text response retained\n");
   }

   /* Unparseable input refuses admission. */
   {
      cJSON *root = cJSON_CreateArray(); /* not an object; backend parse fails */
      aimee_response_t p;
      assert(agent_ir_decode_json_response_bound(root, 1, -1, NULL, NULL, &p) == -1);
      cJSON_Delete(root);
      printf("  PASS: unparseable response fails admission\n");
   }

   /* Rescue runs once at native entry and returns owned canonical calls. */
   {
      const char *resp =
          "{\"model\":\"m\",\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":"
          "\"sure\\n<tool_call><name>bash</name><arguments>{\\\"cmd\\\":\\\"ls\\\"}</arguments>"
          "</tool_call>\"},\"finish_reason\":\"stop\"}]}";
      cJSON *root = cJSON_Parse(resp);
      assert(root);
      aimee_response_t p;
      int nr = -1;
      /* rescue_mode 0 = rescue dialect calls */
      assert(agent_ir_decode_json_response_bound(root, 0, 0, &nr, NULL, &p) == 0);
      assert(nr == 1);                                 /* one call rescued */
      assert(aimee_ir_response_has_tool_use(&p) == 1); /* now a tool call */
      assert(aimee_ir_response_tool_count(&p) == 1 &&
             strcmp(aimee_ir_response_tool(&p, 0)->tool_name, "bash") == 0);
      assert(p.n_content == 2 && p.content[0].type == AIMEE_BLK_TEXT &&
             p.content[1].type == AIMEE_BLK_TOOL_USE);
      assert_ir_text(&p, "sure");
      assert(!strcmp(
          cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(p.content[1].tool_input, "cmd")),
          "ls"));
      aimee_response_free(&p);
      cJSON_Delete(root);
      printf("  PASS: parser owns XML rescue (text -> tool call, n_rescued reported)\n");
   }

   /* Refused partial text must survive the actual execution entry without
    * acquiring rescue tool authority, with or without optional refusal details. */
   for (int detailed = 0; detailed < 2; detailed++)
   {
      cJSON *root =
          cJSON_Parse("{\"model\":\"m\",\"stop_reason\":\"refusal\",\"content\":["
                      "{\"type\":\"text\",\"text\":\"<tool_call><name>bash</name><arguments>"
                      "{\\\"command\\\":\\\"ls\\\"}</arguments></tool_call>\"}]}");
      assert(root);
      if (detailed)
      {
         cJSON *details = cJSON_AddObjectToObject(root, "stop_details");
         cJSON_AddStringToObject(details, "type", "refusal");
         cJSON_AddStringToObject(details, "category", "future_category");
         cJSON_AddStringToObject(details, "explanation", "provider diagnostic");
      }
      aimee_response_t response = {0};
      int rescued = -1;
      assert(agent_ir_decode_json_response_bound(root, 1, 0, &rescued, NULL, &response) == 0);
      cJSON_Delete(root);
      assert(rescued == 0 && !aimee_ir_response_tool_count(&response));
      assert(response.stop_reason == AIMEE_STOP_CONTENT_FILTER);
      assert(response.raw_stop_reason && !strcmp(response.raw_stop_reason, "refusal"));
      assert(response.n_content == 1 && response.content[0].type == AIMEE_BLK_TEXT);
      assert(strstr(response.content[0].text, "<tool_call>"));
      if (detailed)
         assert(response.refusal && !strcmp(response.refusal->category, "future_category"));
      aimee_response_free(&response);
   }
   puts("  PASS: canonical refusal cannot become a rescued executable call");

   /* 6. rescue_mode < 0 disables the rescue: the same embedded <tool_call> stays as
    * text, not a tool call. */
   {
      const char *resp =
          "{\"model\":\"m\",\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":"
          "\"<tool_call><name>bash</name><arguments>{}</arguments></tool_call>\"}}]}";
      cJSON *root = cJSON_Parse(resp);
      aimee_response_t p;
      int nr = -1;
      assert(agent_ir_decode_json_response_bound(root, 0, -1 /*no rescue*/, &nr, NULL, &p) == 0);
      assert(nr == 0 && aimee_ir_response_has_tool_use(&p) == 0);
      aimee_response_free(&p);
      cJSON_Delete(root);
      printf("  PASS: rescue_mode<0 leaves embedded calls as text\n");
   }

   /* The observation tap leaves canonical reasoning and visible text intact. */
   {
      /* Anthropic wire: a thinking block is observed, and the answer text is
       * unaffected by the tap. */
      aimee_ir_metrics_reset();
      const char *resp = "{\"model\":\"m\",\"stop_reason\":\"end_turn\",\"content\":["
                         "{\"type\":\"thinking\",\"thinking\":\"weighing it up\","
                         "\"signature\":\"sig\"},"
                         "{\"type\":\"text\",\"text\":\"the answer\"}],"
                         "\"usage\":{\"input_tokens\":3,\"output_tokens\":4}}";
      cJSON *root = cJSON_Parse(resp);
      assert(root);
      aimee_response_t p;
      assert(agent_ir_decode_json_response_bound(root, 1, -1, NULL, NULL, &p) == 0);
      assert(aimee_ir_metric_total(AIMEE_IR_M_REASONING_OBSERVED) == 1);
      assert(aimee_ir_metric_total(AIMEE_IR_M_REASONING_INCOMPLETE) == 0);
      /* the tap observes; it must not disturb what the turn loop consumes */
      assert_ir_text(&p, "the answer");
      aimee_response_free(&p);
      cJSON_Delete(root);
      printf("  PASS: reasoning observed on the buffered wire\n");
   }

   {
      /* A turn with no reasoning must move neither counter, or the base rate the
       * counters exist to measure is meaningless. */
      aimee_ir_metrics_reset();
      const char *resp = "{\"model\":\"m\",\"stop_reason\":\"end_turn\","
                         "\"content\":[{\"type\":\"text\",\"text\":\"just an answer\"}]}";
      cJSON *root = cJSON_Parse(resp);
      assert(root);
      aimee_response_t p;
      assert(agent_ir_decode_json_response_bound(root, 1, -1, NULL, NULL, &p) == 0);
      assert(aimee_ir_metric_total(AIMEE_IR_M_REASONING_OBSERVED) == 0);
      assert(aimee_ir_metric_total(AIMEE_IR_M_REASONING_INCOMPLETE) == 0);
      aimee_response_free(&p);
      cJSON_Delete(root);
      printf("  PASS: a turn without reasoning moves no counter\n");
   }

   {
      /* A thinking block that arrives EMPTY counts as incomplete, never observed:
       * "the reasoning was dropped upstream" must not read as "there was none". */
      aimee_ir_metrics_reset();
      const char *resp = "{\"model\":\"m\",\"stop_reason\":\"end_turn\",\"content\":["
                         "{\"type\":\"thinking\",\"thinking\":\"\"},"
                         "{\"type\":\"text\",\"text\":\"a\"}]}";
      cJSON *root = cJSON_Parse(resp);
      assert(root);
      aimee_response_t p;
      assert(agent_ir_decode_json_response_bound(root, 1, -1, NULL, NULL, &p) == 0);
      assert(aimee_ir_metric_total(AIMEE_IR_M_REASONING_OBSERVED) == 0);
      assert(aimee_ir_metric_total(AIMEE_IR_M_REASONING_INCOMPLETE) == 1);
      aimee_response_free(&p);
      cJSON_Delete(root);
      aimee_ir_metrics_reset();
      printf("  PASS: an empty thinking block counts as incomplete, not observed\n");
   }

   printf("agent-ir-parse: ok\n");
   return 0;
}
