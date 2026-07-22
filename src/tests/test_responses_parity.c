/* test_responses_parity.c -- golden tests for the IR responses parser
 * (agent_ir_parse_responses, the sole parser for the responses/SSE codex wire).
 * Originally a legacy-vs-IR differential; the legacy agent_parse_response_responses
 * has since been deleted, so this pins the IR output against known-good values across
 * the shapes codex emits: reasoning-only, message output_text, delta-streamed text
 * (the fold path), function_call, and a mixed reasoning+message turn.
 *
 * Own binary with the minimal real object set (agent_bridge.o for the SSE extractor +
 * agent_ir_parse.o + the IR backend chain, no weak stubs) so the parser runs for real. */
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

/* Parse `body` via the IR and assert the fields the turn loop consumes. want_content is
 * compared as empty when NULL. want_call (or NULL) is the single expected tool name. */
static void expect(const char *label, const char *body, const char *want_content,
                   const char *want_call)
{
   parsed_response_t p;
   int rc = agent_ir_parse_responses(body, -1, NULL, &p);
   assert(rc == 0);

   const char *got = p.content ? p.content : "";
   assert(strcmp(got, want_content ? want_content : "") == 0);
   if (want_call)
   {
      assert(p.is_tool_call == 1 && p.call_count == 1);
      assert(strcmp(p.calls[0].name, want_call) == 0);
   }
   else
   {
      assert(p.is_tool_call == 0 && p.call_count == 0);
   }
   printf("  PASS: %s (content_len=%zu tool=%d)\n", label, strlen(got), p.is_tool_call);
   agent_free_parsed_response(&p);
}

int main(void)
{
   printf("responses-parity:\n");

   /* reasoning-only final turn -> empty content (reasoning routes to a THINKING block,
    * excluded from content). This is the "no content in final response" case: codex
    * model behavior, faithfully represented, not a parse failure. */
   expect("reasoning-only -> empty content",
          "event: response.output_item.done\r\n"
          "data: {\"item\":{\"type\":\"reasoning\",\"summary\":\"let me think\"}}\r\n\r\n"
          "event: response.completed\r\n"
          "data: {\"response\":{\"output\":[{\"type\":\"reasoning\",\"summary\":\"let me "
          "think\"}],\"usage\":{\"input_tokens\":5,\"output_tokens\":6}}}\r\n\r\n",
          NULL, NULL);

   expect("message output_text",
          "event: response.output_item.done\r\n"
          "data: {\"item\":{\"type\":\"message\",\"content\":[{\"type\":\"output_text\","
          "\"text\":\"all done\"}]}}\r\n\r\n"
          "event: response.completed\r\n"
          "data: {\"response\":{\"output\":[{\"type\":\"message\",\"content\":[{\"type\":"
          "\"output_text\",\"text\":\"all done\"}]}],\"usage\":{\"input_tokens\":3,"
          "\"output_tokens\":4}}}\r\n\r\n",
          "all done", NULL);

   expect("delta-streamed text (fold)",
          "event: response.output_text.delta\r\n"
          "data: {\"delta\":\"streamed answer\"}\r\n\r\n"
          "event: response.completed\r\n"
          "data: {\"response\":{\"output\":[],\"usage\":{\"input_tokens\":1,"
          "\"output_tokens\":2}}}\r\n\r\n",
          "streamed answer", NULL);

   expect("function_call",
          "event: response.output_item.done\r\n"
          "data: {\"item\":{\"type\":\"function_call\",\"call_id\":\"c9\",\"name\":\"bash\","
          "\"arguments\":\"{\\\"cmd\\\":\\\"ls\\\"}\"}}\r\n\r\n"
          "event: response.completed\r\n"
          "data: {\"response\":{\"output\":[{\"type\":\"function_call\",\"call_id\":\"c9\","
          "\"name\":\"bash\",\"arguments\":\"{\\\"cmd\\\":\\\"ls\\\"}\"}],"
          "\"usage\":{\"input_tokens\":7,\"output_tokens\":8}}}\r\n\r\n",
          "", "bash");

   expect("reasoning + message text",
          "event: response.output_item.done\r\n"
          "data: {\"item\":{\"type\":\"reasoning\",\"summary\":\"hmm\"}}\r\n\r\n"
          "event: response.output_item.done\r\n"
          "data: {\"item\":{\"type\":\"message\",\"content\":[{\"type\":\"output_text\","
          "\"text\":\"result 42\"}]}}\r\n\r\n"
          "event: response.completed\r\n"
          "data: {\"response\":{\"output\":[{\"type\":\"reasoning\",\"summary\":\"hmm\"},"
          "{\"type\":\"message\",\"content\":[{\"type\":\"output_text\",\"text\":\"result 42\"}]}],"
          "\"usage\":{\"input_tokens\":9,\"output_tokens\":10}}}\r\n\r\n",
          "result 42", NULL);

   printf("responses-parity: ok\n");
   return 0;
}
