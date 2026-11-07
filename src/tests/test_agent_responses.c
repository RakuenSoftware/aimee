/* test_agent_responses.c: OpenAI /responses SSE/JSON parser tests, split out of
 * test_agent.c to keep that file under the 2000-line hard limit. These functions
 * are declared in test_agent.c and called from its main(); both objects link into
 * the single unit-test-agent binary. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "aimee.h"
#include "agent.h"
#include "agent_protocol.h"
#include "cJSON.h"
#include "support/ir_response_fixture.h"
#include <aimee/ir/session_event.h>

void test_responses_parser_keeps_all_output_text_parts(void)
{
   const char *body =
       "event: response.output_item.done\n"
       "data: {\"item\":{\"type\":\"message\",\"content\":["
       "{\"type\":\"output_text\",\"text\":\"I did not deploy this to `192.\"},"
       "{\"type\":\"output_text\",\"text\":\"168.0.83`.\"}]}}\n\n"
       "event: response.completed\n"
       "data: {\"response\":{\"usage\":{\"input_tokens\":11,\"output_tokens\":22}}}\n\n";

   aimee_response_t parsed;
   agent_ir_decode_responses(body, -1, NULL, &parsed);
   assert_ir_text(&parsed, "I did not deploy this to `192.168.0.83`.");
   assert(parsed.usage_in == 11);
   assert(parsed.usage_out == 22);
   aimee_response_free(&parsed);
}

void test_responses_parser_accumulates_output_text_deltas(void)
{
   const char *body =
       "event: response.output_text.delta\r\n"
       "data: {\"delta\":\"The useful model fact here is that Qwen3.\"}\r\n\r\n"
       "event: response.output_text.delta\r\n"
       "data: {\"delta\":\"6 keeps scaling KV cache with context length.\"}\r\n\r\n"
       "event: response.completed\r\n"
       "data: {\"response\":{\"output\":[],\"usage\":{\"input_tokens\":5,\"output_tokens\":6}}}"
       "\r\n\r\n";

   aimee_response_t parsed;
   agent_ir_decode_responses(body, -1, NULL, &parsed);
   assert_ir_text(&parsed,
                  "The useful model fact here is that Qwen3.6 keeps scaling KV cache with context "
                  "length.");
   assert(parsed.usage_in == 5);
   assert(parsed.usage_out == 6);
   aimee_response_free(&parsed);
}

/* The active typed decoder must preserve delta text when completed.output is empty. */
void test_responses_object_folds_in_delta_text(void)
{
   const char *body =
       "event: response.output_text.delta\r\n"
       "data: {\"delta\":\"deployed to \"}\r\n\r\n"
       "event: response.output_text.delta\r\n"
       "data: {\"delta\":\"192.168.1.254\"}\r\n\r\n"
       "event: response.completed\r\n"
       "data: {\"response\":{\"output\":[],\"usage\":{\"input_tokens\":5,\"output_tokens\":6}}}"
       "\r\n\r\n";

   aimee_response_t parsed;
   assert(!agent_ir_decode_responses(body, -1, NULL, &parsed));
   assert_ir_text(&parsed, "deployed to 192.168.1.254");
   assert(parsed.usage_in == 5 && parsed.usage_out == 6);
   aimee_response_free(&parsed);
}

/* The codex wire delivers a function_call as a response.output_item.done event and
 * its response.completed event carries "output":[] -- exactly the emptiness the text
 * case above handles. The streamed call was collected and then discarded, so
 * responses_backend_parse saw no tool call and the delegate reported
 * "no content in final response" with tool_calls=0 on every attempt, at turn 1,
 * forever. Measured live: item_types=[reasoning,function_call] on the wire, zero
 * tool calls extracted. Fold the streamed call back in, as the text is. */
void test_responses_object_folds_in_streamed_function_call(void)
{
   const char *body =
       "event: response.output_item.done\r\n"
       "data: {\"item\":{\"type\":\"reasoning\",\"summary\":\"thinking\"}}\r\n\r\n"
       "event: response.output_item.done\r\n"
       "data: {\"item\":{\"type\":\"function_call\",\"call_id\":\"call_42\","
       "\"name\":\"read_file\",\"arguments\":\"{\\\"path\\\":\\\"/fixture/x\\\"}\"}}\r\n\r\n"
       "event: response.completed\r\n"
       "data: {\"response\":{\"output\":[],\"usage\":{\"input_tokens\":5,\"output_tokens\":7}}}"
       "\r\n\r\n";

   aimee_response_t parsed;
   assert(!agent_ir_decode_responses(body, -1, NULL, &parsed));
   assert(aimee_ir_response_tool_count(&parsed) == 1);
   const aimee_block_t *tool = aimee_ir_response_tool(&parsed, 0);
   assert(!strcmp(tool->tool_name, "read_file") && !strcmp(tool->tool_id, "call_42"));
   assert(
       !strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(tool->tool_input, "path")), "/fixture/x"));
   assert(parsed.n_content == 2 && parsed.content[0].type == AIMEE_BLK_THINKING);
   assert(!strcmp(parsed.content[0].text, "thinking"));
   assert(parsed.usage_in == 5 && parsed.usage_out == 7);
   aimee_response_free(&parsed);
}

/* Never duplicate a call the completed payload already carries -- that payload is
 * authoritative whenever it is populated, and a doubled call would be executed twice. */
void test_responses_object_keeps_existing_function_call(void)
{
   const char *body =
       "event: response.output_item.done\r\n"
       "data: {\"item\":{\"type\":\"function_call\",\"call_id\":\"call_7\","
       "\"name\":\"list_files\",\"arguments\":\"{}\"}}\r\n\r\n"
       "event: response.completed\r\n"
       "data: {\"response\":{\"output\":[{\"type\":\"function_call\",\"call_id\":\"call_7\","
       "\"name\":\"list_files\",\"arguments\":\"{}\"}],\"usage\":{\"input_tokens\":1,"
       "\"output_tokens\":2}}}\r\n\r\n";

   aimee_response_t parsed;
   assert(!agent_ir_decode_responses(body, -1, NULL, &parsed));
   assert(aimee_ir_response_tool_count(&parsed) == 1);
   const aimee_block_t *tool = aimee_ir_response_tool(&parsed, 0);
   assert(!strcmp(tool->tool_name, "list_files") && !strcmp(tool->tool_id, "call_7"));
   assert(parsed.usage_in == 1 && parsed.usage_out == 2);
   aimee_response_free(&parsed);
}

/* Guard against double-injection: when the completed object ALREADY carries the
 * message text (non-codex responses that repeat it in output[]), the extractor must
 * leave a single output_text -- not append a duplicate. */
void test_responses_object_keeps_existing_text(void)
{
   const char *body = "event: response.output_text.delta\r\n"
                      "data: {\"delta\":\"hello there\"}\r\n\r\n"
                      "event: response.completed\r\n"
                      "data: {\"response\":{\"output\":[{\"type\":\"message\",\"content\":["
                      "{\"type\":\"output_text\",\"text\":\"hello there\"}]}],"
                      "\"usage\":{\"input_tokens\":1,\"output_tokens\":2}}}\r\n\r\n";

   aimee_response_t parsed;
   assert(!agent_ir_decode_responses(body, -1, NULL, &parsed));
   assert(parsed.n_content == 1 && parsed.content[0].type == AIMEE_BLK_TEXT);
   assert_ir_text(&parsed, "hello there");
   assert(parsed.usage_in == 1 && parsed.usage_out == 2);
   aimee_response_free(&parsed);
}

/* Native function calls become owned canonical tool blocks for durable replay. */
void test_ir_parse_responses_tool_call(void)
{
   const char *body =
       "event: response.output_item.done\r\n"
       "data: {\"item\":{\"type\":\"function_call\",\"call_id\":\"call_1\",\"name\":\"bash\","
       "\"arguments\":\"{\\\"cmd\\\":\\\"ls\\\"}\"}}\r\n\r\n"
       "event: response.completed\r\n"
       "data: {\"response\":{\"output\":[{\"type\":\"function_call\",\"call_id\":\"call_1\","
       "\"name\":\"bash\",\"arguments\":\"{\\\"cmd\\\":\\\"ls\\\"}\"}],"
       "\"usage\":{\"input_tokens\":3,\"output_tokens\":4}}}\r\n\r\n";

   aimee_response_t p;
   int rc = agent_ir_decode_responses(body, -1, NULL, &p);
   assert(rc == 0);
   assert(aimee_ir_response_has_tool_use(&p) == 1);
   assert(aimee_ir_response_tool_count(&p) == 1);
   assert(strcmp(aimee_ir_response_tool(&p, 0)->tool_name, "bash") == 0);
   assert(strcmp(aimee_ir_response_tool(&p, 0)->tool_id, "call_1") == 0);
   /* The durable checkpoint uses the canonical ID, without a native output array. */
   aimee_message_t message = {.role = "assistant", .blocks = p.content, .n_blocks = p.n_content};
   cJSON *checkpoint = aimee_ir_message_json(&message);
   assert(checkpoint && !cJSON_GetObjectItemCaseSensitive(checkpoint, "output"));
   const cJSON *block =
       cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(checkpoint, "blocks"), 0);
   assert(
       !strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(block, "tool_id")), "call_1"));
   cJSON_Delete(checkpoint);
   assert(!aimee_ir_response_tool(&p, 0)->tool_namespace);
   aimee_response_free(&p);
}

/* A tool the client offered inside a `namespace` group comes back with a BARE
 * nested name and the group beside it. Both halves have to reach the parsed call:
 * the client routes on the pair and answers "unsupported call: git" given only the
 * name. */
void test_ir_parse_responses_namespaced_tool_call(void)
{
   const char *body =
       "event: response.output_item.done\r\n"
       "data: {\"item\":{\"type\":\"function_call\",\"call_id\":\"call_7\",\"name\":\"git\","
       "\"namespace\":\"mcp__aimee\",\"arguments\":\"{\\\"command\\\":\\\"status\\\"}\"}}\r\n\r\n"
       "event: response.completed\r\n"
       "data: {\"response\":{\"output\":[{\"type\":\"function_call\",\"call_id\":\"call_7\","
       "\"name\":\"git\",\"namespace\":\"mcp__aimee\","
       "\"arguments\":\"{\\\"command\\\":\\\"status\\\"}\"}],"
       "\"usage\":{\"input_tokens\":3,\"output_tokens\":4}}}\r\n\r\n";

   aimee_response_t p;
   assert(agent_ir_decode_responses(body, -1, NULL, &p) == 0);
   assert(aimee_ir_response_tool_count(&p) == 1);
   /* the name stays bare -- it is the group that qualifies it */
   assert(strcmp(aimee_ir_response_tool(&p, 0)->tool_name, "git") == 0);
   assert(strcmp(aimee_ir_response_tool(&p, 0)->tool_namespace, "mcp__aimee") == 0);
   aimee_response_free(&p);
}

/* Text-only Codex reply retains canonical text and full usage. */
void test_ir_parse_responses_text_only(void)
{
   const char *body =
       "event: response.output_text.delta\r\n"
       "data: {\"delta\":\"hello world\"}\r\n\r\n"
       "event: response.completed\r\n"
       "data: {\"response\":{\"output\":[],\"usage\":{\"input_tokens\":1,\"output_tokens\":2}}}"
       "\r\n\r\n";

   aimee_response_t p;
   int rc = agent_ir_decode_responses(body, -1, NULL, &p);
   assert(rc == 0);
   assert(aimee_ir_response_has_tool_use(&p) == 0);
   assert_ir_text(&p, "hello world");
   assert(p.n_content == 1 && p.content[0].type == AIMEE_BLK_TEXT);
   assert(p.usage_in == 1 && p.usage_out == 2);
   aimee_response_free(&p);
}

static void assert_reasoning_content_does_not_hide_answer_events(void)
{
   const char *answer_events[] = {
       "event: response.output_text.delta\ndata: "
       "{\"type\":\"response.output_text.delta\",\"delta\":\"HelloWorld\"}\n\n",
       "event: response.output_text.done\ndata: "
       "{\"type\":\"response.output_text.done\",\"text\":\"HelloWorld\"}\n\n",
       "event: response.content_part.done\ndata: "
       "{\"type\":\"response.content_part.done\",\"part\":{\"type\":\"output_text\",\"text\":"
       "\"HelloWorld\"}}\n\n",
       ""};
   const char *reasoning_types[] = {"reasoning_text", "text"};
   for (int reasoning = 0; reasoning < 2; reasoning++)
      for (int event = 0; event < 4; event++)
         for (int snapshot = 0; snapshot < 2; snapshot++)
         {
            char native_reasoning[512], body[4096];
            snprintf(native_reasoning, sizeof native_reasoning,
                     "{\"type\":\"reasoning\",\"id\":\"rs_fixture\",\"summary\":[],"
                     "\"encrypted_content\":\"opaque-encrypted-fixture\",\"content\":[{"
                     "\"type\":\"%s\",\"text\":\"private synthetic thought must stay opaque\"}]}",
                     reasoning_types[reasoning]);
            snprintf(body, sizeof body,
                     "%sevent: response.completed\n"
                     "data: {\"type\":\"response.completed\",\"response\":{"
                     "\"id\":\"fixture-response\",\"status\":\"completed\",\"output\":[%s,{"
                     "\"type\":\"message\",\"id\":\"msg_fixture\",\"role\":\"assistant\","
                     "\"content\":[{\"type\":\"output_text\",\"text\":\"%s\"}]}],"
                     "\"usage\":{\"input_tokens\":5,\"output_tokens\":109,"
                     "\"output_tokens_details\":{\"reasoning_tokens\":103}}}}\n\n",
                     answer_events[event], native_reasoning, snapshot ? "snapshot answer" : "");
            aimee_response_t parsed = {0};
            assert(!agent_ir_decode_responses(body, -1, NULL, &parsed));
            assert_ir_text(&parsed, snapshot ? "snapshot answer" : event < 3 ? "HelloWorld" : "");
            assert(parsed.n_content == (snapshot || event == 3 ? 2 : 3));
            assert(
                parsed.content[0].type == AIMEE_BLK_THINKING && !parsed.content[0].text &&
                !strcmp(parsed.content[0].thinking_id, "rs_fixture") &&
                !strcmp(parsed.content[0].thinking_encrypted_content, "opaque-encrypted-fixture"));
            cJSON *expected = cJSON_Parse(native_reasoning);
            assert(expected && cJSON_Compare(parsed.content[0].raw, expected, 1));
            cJSON_Delete(expected);
            assert(parsed.usage_in == 5 && parsed.usage_out == 109 &&
                   parsed.usage_reasoning == 103);
            assert(parsed.stop_reason == AIMEE_STOP_END_TURN &&
                   !strcmp(parsed.raw_stop_reason, "completed"));
            aimee_response_free(&parsed);
         }
}

void test_responses_parser_uses_output_text_done(void)
{
   assert_reasoning_content_does_not_hide_answer_events();
   const char *body =
       "event: response.content_part.done\n"
       "data: {\"part\":{\"type\":\"output_text\",\"text\":\"One caveat: the endpoint I could "
       "reach at `192.168.1.103:8080`.\"}}\n\n"
       "event: response.output_text.done\n"
       "data: {\"text\":\"One caveat: the endpoint I could reach at `192.168.1.103:8080`.\"}\n\n"
       "event: response.completed\n"
       "data: "
       "{\"response\":{\"output\":[],\"usage\":{\"input_tokens\":7,\"output_tokens\":8}}}\n\n";

   aimee_response_t parsed;
   agent_ir_decode_responses(body, -1, NULL, &parsed);
   assert_ir_text(&parsed, "One caveat: the endpoint I could reach at `192.168.1.103:8080`.");
   assert(parsed.usage_in == 7);
   assert(parsed.usage_out == 8);
   aimee_response_free(&parsed);
}

void test_responses_parser_separates_message_items(void)
{
   const char *body =
       "event: response.output_text.delta\n"
       "data: {\"delta\":\"PR.\"}\n\n"
       "event: response.output_text.delta\n"
       "data: {\"delta\":\"GitHub\"}\n\n"
       "event: response.output_item.done\n"
       "data: {\"item\":{\"type\":\"message\",\"content\":["
       "{\"type\":\"output_text\",\"text\":\"PR.\"}]}}\n\n"
       "event: response.output_item.done\n"
       "data: {\"item\":{\"type\":\"message\",\"content\":["
       "{\"type\":\"output_text\",\"text\":\"GitHub\"}]}}\n\n"
       "event: response.completed\n"
       "data: {\"response\":{\"output\":[],\"usage\":{\"input_tokens\":9,\"output_tokens\":10}}}"
       "\n\n";

   aimee_response_t parsed;
   agent_ir_decode_responses(body, -1, NULL, &parsed);
   assert_ir_text(&parsed, "PR.\n\nGitHub");
   assert(parsed.usage_in == 9);
   assert(parsed.usage_out == 10);
   aimee_response_free(&parsed);
}
