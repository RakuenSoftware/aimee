/* test_agent_ir_parse.c -- agent_ir_parse_json_response: the IR-backed response
 * parser that is now the default for delegate turns. The shadow validated content +
 * tool-call parity on live traffic; this pins the bridge's own output shape --
 * especially assistant_message (the multi-turn replay), which the shadow does not
 * cover -- so a regression in the conversation-history contract is caught here. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "aimee.h"

#include "agent_protocol.h"
#include "cJSON.h"

/* Local copy of agent_free_parsed_response (agent_bridge.c) so the test stays
 * self-contained rather than linking the whole legacy bridge. Same semantics. */
/* Stub the tool registry: the dialect rescue consults it only on bare-JSON/bracket
 * paths, which these explicit <tool_call> cases do not hit -- same pattern as
 * test_delegate_xml_fallback.c. */
struct cJSON *agent_tool_get_schema_cached(const char *tool_name)
{
   (void)tool_name;
   return NULL;
}

void agent_free_parsed_response(parsed_response_t *p)
{
   if (!p)
      return;
   for (int i = 0; i < p->call_count; i++)
      free(p->calls[i].arguments);
   free(p->content);
   if (p->assistant_message)
      cJSON_Delete(p->assistant_message);
   memset(p, 0, sizeof(*p));
}

int main(void)
{
   printf("agent-ir-parse:\n");

   /* 1. Anthropic: text AND a tool_use in one turn. Both survive; assistant_message
    * is the raw content array (what the anthropic multi-turn append re-wraps). */
   {
      const char *resp =
          "{\"model\":\"m\",\"stop_reason\":\"tool_use\",\"content\":["
          "{\"type\":\"text\",\"text\":\"Let me check.\"},"
          "{\"type\":\"tool_use\",\"id\":\"t1\",\"name\":\"bash\",\"input\":{\"cmd\":\"ls\"}}],"
          "\"usage\":{\"input_tokens\":11,\"output_tokens\":7}}";
      cJSON *root = cJSON_Parse(resp);
      assert(root);
      parsed_response_t p;
      int rc = agent_ir_parse_json_response(root, 1 /*anthropic*/, -1, NULL, &p);
      assert(rc == 0);
      assert(p.is_tool_call == 1);
      assert(p.call_count == 1 && strcmp(p.calls[0].name, "bash") == 0);
      assert(strcmp(p.calls[0].id, "t1") == 0);
      assert(p.content && strcmp(p.content, "Let me check.") == 0); /* text preserved */
      assert(p.prompt_tokens == 11 && p.completion_tokens == 7);
      assert(strcmp(p.stop_reason, "tool_use") == 0);
      /* assistant_message == the content array (2 blocks), for anthropic replay */
      assert(p.assistant_message && cJSON_IsArray(p.assistant_message));
      assert(cJSON_GetArraySize(p.assistant_message) == 2);
      /* the tool arguments round-trip to the input object */
      cJSON *a = cJSON_Parse(p.calls[0].arguments);
      assert(a && strcmp(cJSON_GetObjectItem(a, "cmd")->valuestring, "ls") == 0);
      cJSON_Delete(a);
      agent_free_parsed_response(&p);
      cJSON_Delete(root);
      printf("  PASS: anthropic text+tool_use bridged (text kept, assistant_message = content)\n");
   }

   /* 2. OpenAI: a tool_calls response. assistant_message is the choice message
    * object (with tool_calls), which the openai multi-turn append reads. */
   {
      const char *resp =
          "{\"model\":\"m\",\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":null,"
          "\"tool_calls\":[{\"id\":\"c1\",\"type\":\"function\",\"function\":{\"name\":\"grep\","
          "\"arguments\":\"{\\\"q\\\":\\\"x\\\"}\"}}]},\"finish_reason\":\"tool_calls\"}],"
          "\"usage\":{\"prompt_tokens\":5,\"completion_tokens\":3}}";
      cJSON *root = cJSON_Parse(resp);
      assert(root);
      parsed_response_t p;
      int rc = agent_ir_parse_json_response(root, 0 /*openai*/, -1, NULL, &p);
      assert(rc == 0);
      assert(p.is_tool_call == 1 && p.call_count == 1);
      assert(strcmp(p.calls[0].name, "grep") == 0);
      /* assistant_message is the message object carrying tool_calls (for replay) */
      assert(p.assistant_message && cJSON_IsObject(p.assistant_message));
      assert(cJSON_GetObjectItem(p.assistant_message, "tool_calls") != NULL);
      agent_free_parsed_response(&p);
      cJSON_Delete(root);
      printf("  PASS: openai tool_calls bridged (assistant_message = choice message)\n");
   }

   /* 3. Plain anthropic text (no tool call): content set, not a tool call, and no
    * assistant_message (anthropic only replays it on a tool turn). */
   {
      const char *resp = "{\"model\":\"m\",\"stop_reason\":\"end_turn\",\"content\":["
                         "{\"type\":\"text\",\"text\":\"done\"}]}";
      cJSON *root = cJSON_Parse(resp);
      parsed_response_t p;
      assert(agent_ir_parse_json_response(root, 1, -1, NULL, &p) == 0);
      assert(p.is_tool_call == 0 && p.content && strcmp(p.content, "done") == 0);
      assert(p.assistant_message == NULL);
      agent_free_parsed_response(&p);
      cJSON_Delete(root);
      printf("  PASS: plain text response bridged\n");
   }

   /* 4. Unparseable -> -1 so the caller can fall back to the legacy translator. */
   {
      cJSON *root = cJSON_CreateArray(); /* not an object; backend parse fails */
      parsed_response_t p;
      assert(agent_ir_parse_json_response(root, 1, -1, NULL, &p) == -1);
      cJSON_Delete(root);
      printf("  PASS: unparseable response returns -1 (legacy fallback)\n");
   }

   /* 5. XML rescue OWNED by the parser: a response with no native tool call but an
    * embedded <tool_call> in text is rescued into a real tool call, n_rescued
    * reports it, and (for openai) assistant_message is rebuilt from the call. */
   {
      const char *resp =
          "{\"model\":\"m\",\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":"
          "\"sure\\n<tool_call><name>bash</name><arguments>{\\\"cmd\\\":\\\"ls\\\"}</arguments>"
          "</tool_call>\"}}]}";
      cJSON *root = cJSON_Parse(resp);
      assert(root);
      parsed_response_t p;
      int nr = -1;
      /* rescue_mode 0 = rescue dialect calls */
      assert(agent_ir_parse_json_response(root, 0, 0, &nr, &p) == 0);
      assert(nr == 1);             /* one call rescued */
      assert(p.is_tool_call == 1); /* now a tool call */
      assert(p.call_count == 1 && strcmp(p.calls[0].name, "bash") == 0);
      /* openai rescued turn: assistant_message rebuilt from the call (has tool_calls) */
      assert(p.assistant_message && cJSON_GetObjectItem(p.assistant_message, "tool_calls"));
      agent_free_parsed_response(&p);
      cJSON_Delete(root);
      printf("  PASS: parser owns XML rescue (text -> tool call, n_rescued reported)\n");
   }

   /* 6. rescue_mode < 0 disables the rescue: the same embedded <tool_call> stays as
    * text, not a tool call. */
   {
      const char *resp =
          "{\"model\":\"m\",\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":"
          "\"<tool_call><name>bash</name><arguments>{}</arguments></tool_call>\"}}]}";
      cJSON *root = cJSON_Parse(resp);
      parsed_response_t p;
      int nr = -1;
      assert(agent_ir_parse_json_response(root, 0, -1 /*no rescue*/, &nr, &p) == 0);
      assert(nr == 0 && p.is_tool_call == 0);
      agent_free_parsed_response(&p);
      cJSON_Delete(root);
      printf("  PASS: rescue_mode<0 leaves embedded calls as text\n");
   }

   printf("agent-ir-parse: ok\n");
   return 0;
}
