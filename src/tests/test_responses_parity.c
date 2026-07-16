/* test_responses_parity.c -- deterministic differential proof that the IR responses
 * parser (agent_ir_parse_responses, stage 2 of the legacy-parser removal) produces the
 * SAME parsed_response_t as the legacy agent_parse_response_responses on identical codex
 * SSE bytes: content, tool calls, and token usage. This replaces live-traffic sampling
 * as the codex-wire parity check.
 *
 * Own binary (not folded into unit-test-agent) on purpose: it links the minimal real
 * object set -- agent_bridge.o (legacy parser + SSE extractor) + agent_ir_parse.o + the
 * IR backend chain, no weak stubs -- so both parsers run for real with no link-order
 * ambiguity. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "aimee.h"
#include "agent_protocol.h"
#include "cJSON.h"

/* The dialect rescue consults the tool registry only on bare-JSON/bracket paths, which
 * these explicit cases never hit -- stub it (same pattern as test_agent_ir_parse.c). */
struct cJSON *agent_tool_get_schema_cached(const char *tool_name)
{
   (void)tool_name;
   return NULL;
}

/* Feed identical SSE bytes to both parsers; assert they agree on the fields the turn
 * loop consumes. */
static void agree(const char *label, const char *body)
{
   parsed_response_t lg, ir;
   agent_parse_response_responses(body, &lg);
   int rc = agent_ir_parse_responses(body, -1, NULL, &ir);
   assert(rc == 0);

   const char *lc = lg.content ? lg.content : "";
   const char *ic = ir.content ? ir.content : "";
   assert(strcmp(lc, ic) == 0);
   assert((lg.is_tool_call ? 1 : 0) == (ir.is_tool_call ? 1 : 0));
   assert(lg.call_count == ir.call_count);
   for (int i = 0; i < lg.call_count; i++)
   {
      assert(strcmp(lg.calls[i].name, ir.calls[i].name) == 0);
      assert(strcmp(lg.calls[i].id, ir.calls[i].id) == 0);
   }
   assert(lg.prompt_tokens == ir.prompt_tokens);
   assert(lg.completion_tokens == ir.completion_tokens);
   printf("  PASS: %s (content_len=%zu tool=%d calls=%d)\n", label, strlen(ic), ir.is_tool_call,
          ir.call_count);
   agent_free_parsed_response(&lg);
   agent_free_parsed_response(&ir);
}

int main(void)
{
   printf("responses-parity:\n");

   /* reasoning-only final turn: BOTH parsers yield empty content (legacy ignores
    * reasoning items; the IR routes them to THINKING, excluded from content). This is
    * the "no content in final response" case seen on live codex -- identical model
    * behavior under both parsers, NOT an IR regression. */
   agree("reasoning-only -> empty content",
         "event: response.output_item.done\r\n"
         "data: {\"item\":{\"type\":\"reasoning\",\"summary\":\"let me think\"}}\r\n\r\n"
         "event: response.completed\r\n"
         "data: {\"response\":{\"output\":[{\"type\":\"reasoning\","
         "\"summary\":\"let me "
         "think\"}],\"usage\":{\"input_tokens\":5,\"output_tokens\":6}}}\r\n\r\n");

   /* plain text via a message output_text item */
   agree("message output_text",
         "event: response.output_item.done\r\n"
         "data: {\"item\":{\"type\":\"message\",\"content\":[{\"type\":\"output_text\","
         "\"text\":\"all done\"}]}}\r\n\r\n"
         "event: response.completed\r\n"
         "data: {\"response\":{\"output\":[{\"type\":\"message\",\"content\":[{\"type\":"
         "\"output_text\",\"text\":\"all done\"}]}],\"usage\":{\"input_tokens\":3,"
         "\"output_tokens\":4}}}\r\n\r\n");

   /* text streamed via deltas with an empty completed output (the fold path) */
   agree("delta-streamed text (fold)",
         "event: response.output_text.delta\r\n"
         "data: {\"delta\":\"streamed answer\"}\r\n\r\n"
         "event: response.completed\r\n"
         "data: {\"response\":{\"output\":[],\"usage\":{\"input_tokens\":1,"
         "\"output_tokens\":2}}}\r\n\r\n");

   /* a function_call (tool use), streamed as an output_item.done then completed */
   agree("function_call",
         "event: response.output_item.done\r\n"
         "data: {\"item\":{\"type\":\"function_call\",\"call_id\":\"c9\",\"name\":\"bash\","
         "\"arguments\":\"{\\\"cmd\\\":\\\"ls\\\"}\"}}\r\n\r\n"
         "event: response.completed\r\n"
         "data: {\"response\":{\"output\":[{\"type\":\"function_call\",\"call_id\":\"c9\","
         "\"name\":\"bash\",\"arguments\":\"{\\\"cmd\\\":\\\"ls\\\"}\"}],"
         "\"usage\":{\"input_tokens\":7,\"output_tokens\":8}}}\r\n\r\n");

   /* reasoning followed by a text message (mixed final turn) */
   agree("reasoning + message text",
         "event: response.output_item.done\r\n"
         "data: {\"item\":{\"type\":\"reasoning\",\"summary\":\"hmm\"}}\r\n\r\n"
         "event: response.output_item.done\r\n"
         "data: {\"item\":{\"type\":\"message\",\"content\":[{\"type\":\"output_text\","
         "\"text\":\"result 42\"}]}}\r\n\r\n"
         "event: response.completed\r\n"
         "data: {\"response\":{\"output\":[{\"type\":\"reasoning\",\"summary\":\"hmm\"},"
         "{\"type\":\"message\",\"content\":[{\"type\":\"output_text\",\"text\":\"result 42\"}]}],"
         "\"usage\":{\"input_tokens\":9,\"output_tokens\":10}}}\r\n\r\n");

   printf("responses-parity: ok\n");
   return 0;
}
