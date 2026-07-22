/* test_delegate_xml_fallback.c — unit tests for the XML/rescue tool-call parser
 * (src/server/delegate_xml_fallback.c), the layer that lets models without
 * native function-calling still emit tool calls.
 *
 * Includes a regression test for the cross-block attribution bug: a malformed
 * <tool_call> missing its own <name> must NOT borrow the <name> of a later
 * block (which fabricated a tool call mixing one block's name with another's
 * arguments). */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "aimee.h"
#include "agent_protocol.h"
#include "cJSON.h"
#include <aimee/delegates/delegate_xml_fallback.h>

/* The parser references the tool registry only on its bare-JSON/bracket rescue
 * paths (gating unknown tool names). These tests exercise the explicit
 * <tool_call>/<function=> formats, which accept any name, so a NULL stub keeps
 * the test self-contained. */
struct cJSON *agent_tool_get_schema_cached(const char *tool_name)
{
   (void)tool_name;
   return NULL;
}

/* Provide the real symbol the parser's rescue detector links against (and reuse
 * it for test cleanup). Matches agent_protocol.c semantics. */
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

/* ---- 1. basic single tool call ---- */
static void test_basic(void)
{
   parsed_response_t r;
   memset(&r, 0, sizeof(r));
   int n = xml_parse_tool_calls(
       "<tool_call><name>bash</name><arguments>{\"command\":\"ls -la\"}</arguments></tool_call>",
       &r);
   assert(n == 1);
   assert(r.call_count == 1);
   assert(r.is_tool_call == 1);
   assert(strcmp(r.calls[0].name, "bash") == 0);
   assert(strstr(r.calls[0].arguments, "ls -la") != NULL);
   agent_free_parsed_response(&r);
   printf("  basic: ok\n");
}

/* ---- 2. content before the tool call is captured ---- */
static void test_content_prefix(void)
{
   parsed_response_t r;
   memset(&r, 0, sizeof(r));
   int n = xml_parse_tool_calls(
       "Let me list the files.\n"
       "<tool_call><name>bash</name><arguments>{\"command\":\"ls\"}</arguments></tool_call>",
       &r);
   assert(n == 1);
   assert(r.content != NULL);
   assert(strstr(r.content, "Let me list the files.") != NULL);
   agent_free_parsed_response(&r);
   printf("  content_prefix: ok\n");
}

/* ---- 3. two well-formed blocks parse independently ---- */
static void test_two_blocks(void)
{
   parsed_response_t r;
   memset(&r, 0, sizeof(r));
   int n = xml_parse_tool_calls(
       "<tool_call><name>read</name><arguments>{\"path\":\"a\"}</arguments></tool_call>"
       "<tool_call><name>write</name><arguments>{\"path\":\"b\"}</arguments></tool_call>",
       &r);
   assert(n == 2);
   assert(strcmp(r.calls[0].name, "read") == 0);
   assert(strstr(r.calls[0].arguments, "\"a\"") != NULL);
   assert(strcmp(r.calls[1].name, "write") == 0);
   assert(strstr(r.calls[1].arguments, "\"b\"") != NULL);
   agent_free_parsed_response(&r);
   printf("  two_blocks: ok\n");
}

/* ---- 4. REGRESSION: a block missing <name> must not borrow a later block's
 *        name and pair it with this block's arguments ---- */
static void test_cross_block_name_not_borrowed(void)
{
   parsed_response_t r;
   memset(&r, 0, sizeof(r));
   /* Block 1 has <arguments> but NO <name>; block 2 is well-formed. */
   int n = xml_parse_tool_calls(
       "<tool_call><arguments>{\"a\":1}</arguments></tool_call>"
       "<tool_call><name>bash</name><arguments>{\"b\":2}</arguments></tool_call>",
       &r);
   /* Only the well-formed block 2 yields a call; block 1 is skipped, not
    * fabricated from block 2's name + block 1's arguments. */
   assert(n == 1);
   assert(r.call_count == 1);
   assert(strcmp(r.calls[0].name, "bash") == 0);
   assert(strstr(r.calls[0].arguments, "\"b\"") != NULL);
   assert(strstr(r.calls[0].arguments, "\"a\"") == NULL);
   agent_free_parsed_response(&r);
   printf("  cross_block_name_not_borrowed: ok\n");
}

/* ---- 5. tool-name normalization on the explicit <tool_call> path (lowercase +
 *        '-' -> '_', matching the fill_tool_call-based paths) ---- */
static void test_normalize_name(void)
{
   parsed_response_t r;
   memset(&r, 0, sizeof(r));
   /* Bash special-case. */
   assert(xml_parse_tool_calls("<tool_call><name>Bash</name><arguments>{}</arguments></tool_call>",
                               &r) == 1);
   assert(strcmp(r.calls[0].name, "bash") == 0);
   agent_free_parsed_response(&r);

   /* General: lowercase and hyphen-to-underscore. */
   memset(&r, 0, sizeof(r));
   assert(xml_parse_tool_calls(
              "<tool_call><name>Read-File</name><arguments>{}</arguments></tool_call>", &r) == 1);
   assert(strcmp(r.calls[0].name, "read_file") == 0);
   agent_free_parsed_response(&r);
   printf("  normalize_name: ok\n");
}

/* ---- 6. plain text has no tool calls ---- */
static void test_no_tool_call(void)
{
   parsed_response_t r;
   memset(&r, 0, sizeof(r));
   int n = xml_parse_tool_calls("Here is a plain answer with no tool invocation at all.", &r);
   assert(n == 0);
   assert(r.call_count == 0);
   assert(r.is_tool_call == 0);
   agent_free_parsed_response(&r);
   assert(xml_has_tool_calls("just prose") == 0);
   assert(xml_has_tool_calls("do this <tool_call><name>x</name></tool_call>") == 1);
   printf("  no_tool_call: ok\n");
}

/* ---- 7. arguments with a brace inside a JSON string stay intact ---- */
static void test_brace_in_string(void)
{
   parsed_response_t r;
   memset(&r, 0, sizeof(r));
   int n = xml_parse_tool_calls(
       "<tool_call><name>bash</name><arguments>{\"command\":\"echo }\"}</arguments></tool_call>",
       &r);
   assert(n == 1);
   /* The arguments must parse back to valid JSON with the literal "echo }". */
   cJSON *args = cJSON_Parse(r.calls[0].arguments);
   assert(args != NULL);
   cJSON *cmd = cJSON_GetObjectItemCaseSensitive(args, "command");
   assert(cJSON_IsString(cmd) && strcmp(cmd->valuestring, "echo }") == 0);
   cJSON_Delete(args);
   agent_free_parsed_response(&r);
   printf("  brace_in_string: ok\n");
}

int main(void)
{
   printf("delegate_xml_fallback:\n");
   test_basic();
   test_content_prefix();
   test_two_blocks();
   test_cross_block_name_not_borrowed();
   test_normalize_name();
   test_no_tool_call();
   test_brace_in_string();
   printf("All delegate_xml_fallback tests passed.\n");
   return 0;
}
