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
   /* The <tool_call> and <function=> forms accept any name, but the invoke and
    * bare-JSON rescue paths gate on the registry, so the formats exercised below
    * need "bash" and "read" to look declared. Everything else stays unknown, which
    * is what the unknown-tool rescue cases rely on. */
   if (tool_name && (strcmp(tool_name, "bash") == 0 || strcmp(tool_name, "read") == 0))
      return cJSON_CreateObject();
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

/* ---- 8. the other formats the parser claims to recognise ----
 * The suite covered <tool_call> only; these pin the rest so a port has a spec.
 * Each asserts the shape a tool executor actually needs: a name plus an
 * arguments OBJECT. */
static void assert_object_args(const parsed_tool_call_t *tc, const char *what)
{
   cJSON *args = cJSON_Parse(tc->arguments);
   /* cJSON_Parse succeeds on a bare string, so "it parsed" is not the test --
    * an executor needs an object. */
   assert(args != NULL);
   assert(cJSON_IsObject(args));
   (void)what;
   cJSON_Delete(args);
}

static void test_invoke_format(void)
{
   parsed_response_t r;
   memset(&r, 0, sizeof(r));
   int n = xml_parse_tool_calls(
       "<invoke name=\"bash\"><parameter name=\"command\">ls -la</parameter></invoke>", &r);
   assert(n == 1 && r.call_count == 1);
   assert(strcmp(r.calls[0].name, "bash") == 0);
   assert_object_args(&r.calls[0], "invoke");
   assert(strstr(r.calls[0].arguments, "ls -la") != NULL);
   agent_free_parsed_response(&r);
   printf("  invoke: ok\n");
}

static void test_qwen_function_format(void)
{
   /* The qwen <function=>/<parameter=> form is only reached INSIDE a
    * <tool_call> block; bare, it is not recognised at all. */
   parsed_response_t r;
   memset(&r, 0, sizeof(r));
   int n = xml_parse_tool_calls("<tool_call><function=bash>\n<parameter=command>\nls -la\n"
                                "</parameter>\n</function></tool_call>",
                                &r);
   assert(n == 1 && r.call_count == 1);
   assert(strcmp(r.calls[0].name, "bash") == 0);
   assert_object_args(&r.calls[0], "qwen");
   agent_free_parsed_response(&r);

   memset(&r, 0, sizeof(r));
   assert(xml_parse_tool_calls("<function=bash><parameter=command>ls</parameter></function>", &r) ==
          0);
   agent_free_parsed_response(&r);
   printf("  qwen function=: ok\n");
}

static void test_mistral_format(void)
{
   parsed_response_t r;
   memset(&r, 0, sizeof(r));
   int n = xml_parse_tool_calls(
       "[TOOL_CALLS][{\"name\": \"bash\", \"arguments\": {\"command\": \"ls\"}}]", &r);
   assert(n == 1 && r.call_count == 1);
   assert(strcmp(r.calls[0].name, "bash") == 0);
   assert_object_args(&r.calls[0], "mistral");
   agent_free_parsed_response(&r);
   printf("  mistral: ok\n");
}

/* ---- 9. REGRESSION: harmony arguments must be an object ----
 * <|channel>call: passes the INSIDE of the braces to the argument builder, and
 * the "is this already JSON?" guard used a bare cJSON_Parse -- which succeeds on
 * `"command": "ls"` by reading the leading string and stopping. The brace-less
 * text was then handed to the executor as `arguments`, where an object was
 * required. */
static void test_channel_arguments_are_an_object(void)
{
   parsed_response_t r;
   memset(&r, 0, sizeof(r));
   int n = xml_parse_tool_calls("<|channel>call:bash {\"command\": \"ls\"}", &r);
   assert(n == 1 && r.call_count == 1);
   assert(strcmp(r.calls[0].name, "bash") == 0);
   assert_object_args(&r.calls[0], "channel");

   cJSON *args = cJSON_Parse(r.calls[0].arguments);
   const cJSON *command = cJSON_GetObjectItemCaseSensitive(args, "command");
   assert(cJSON_IsString(command) && strcmp(command->valuestring, "ls") == 0);
   cJSON_Delete(args);
   agent_free_parsed_response(&r);

   /* A nested object value must survive too, not collapse to a string. */
   memset(&r, 0, sizeof(r));
   assert(xml_parse_tool_calls("<|channel>call:bash {\"opts\": {\"deep\": true}}", &r) == 1);
   assert_object_args(&r.calls[0], "channel nested");
   agent_free_parsed_response(&r);
   printf("  channel arguments are an object: ok\n");
}

/* ---- 10. REGRESSION: detector and parser must agree ----
 * The detector matches ":tool_call>", so a namespaced block reports "there are
 * tool calls here"; the parser only scans "<tool_call>" and extracts none. A
 * caller told a response holds tool calls, then handed zero, either drops the
 * call or retries forever. */
static void test_detector_and_parser_agree(void)
{
   static const char *cases[] = {
       "<tool_call><name>bash</name><arguments>{\"a\":1}</arguments></tool_call>",
       "<tools:tool_call><name>bash</name><arguments>{\"a\":1}</arguments></tools:tool_call>",
       "<invoke name=\"bash\"><parameter name=\"command\">ls</parameter></invoke>",
       "[TOOL_CALLS][{\"name\": \"bash\", \"arguments\": {}}]",
       "<|channel>call:bash {\"a\": 1}",
   };
   for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
   {
      parsed_response_t r;
      memset(&r, 0, sizeof(r));
      int parsed = xml_parse_tool_calls(cases[i], &r);
      int detected = delegate_rescue_has_tool_calls(cases[i]);
      /* Detection without extraction is the failure mode being pinned. */
      assert(!(detected && parsed == 0));
      agent_free_parsed_response(&r);
   }
   printf("  detector/parser agree: ok\n");
}

/* ---- 11. bare-JSON rescue, which is gated on the tool registry ---- */
static void test_bare_json_rescue(void)
{
   parsed_response_t r;
   memset(&r, 0, sizeof(r));
   int n = delegate_rescue_parse_tool_calls(
       "{\"name\": \"bash\", \"arguments\": {\"command\": \"ls\"}}", &r, 1);
   assert(n == 1 && r.call_count == 1);
   assert(strcmp(r.calls[0].name, "bash") == 0);
   assert_object_args(&r.calls[0], "bare json");
   agent_free_parsed_response(&r);

   /* An UNKNOWN tool name is not rescued: bare JSON is ambiguous with ordinary
    * prose containing an object, so the registry is what makes it a call. */
   memset(&r, 0, sizeof(r));
   assert(delegate_rescue_parse_tool_calls("{\"name\": \"nope\", \"arguments\": {}}", &r, 1) == 0);
   assert(delegate_rescue_has_tool_calls_with_json("{\"name\": \"nope\", \"arguments\": {}}", 1) ==
          0);
   agent_free_parsed_response(&r);

   /* allow_json=0 turns the whole rescue off, even for a known tool. */
   memset(&r, 0, sizeof(r));
   assert(delegate_rescue_parse_tool_calls("{\"name\": \"bash\", \"arguments\": {\"a\":1}}", &r,
                                           0) == 0);
   agent_free_parsed_response(&r);

   /* An array of calls is rescued as several. */
   memset(&r, 0, sizeof(r));
   n = delegate_rescue_parse_tool_calls("[{\"name\": \"bash\", \"arguments\": {\"a\":1}}, "
                                        "{\"name\": \"read\", \"arguments\": {\"b\":2}}]",
                                        &r, 1);
   assert(n == 2 && r.call_count == 2);
   assert(strcmp(r.calls[0].name, "bash") == 0 && strcmp(r.calls[1].name, "read") == 0);
   agent_free_parsed_response(&r);
   printf("  bare json rescue: ok\n");
}

/* ---- 12. REGRESSION: the tool/parameters spelling must carry its arguments ----
 * The name is accepted as either "tool" or "name", so the tool/parameters
 * convention is deliberately supported -- but arguments were read only from
 * "args"/"arguments". A model emitting {"tool": ..., "parameters": {...}} had its
 * call invoked with an EMPTY argument object, which is worse than not invoking
 * it: a bash call arrives with no command rather than being left alone. */
static void test_tool_parameters_spelling(void)
{
   parsed_response_t r;
   memset(&r, 0, sizeof(r));
   int n = delegate_rescue_parse_tool_calls(
       "{\"tool\": \"bash\", \"parameters\": {\"command\": \"ls -la\"}}", &r, 1);
   assert(n == 1 && r.call_count == 1);
   assert(strcmp(r.calls[0].name, "bash") == 0);
   assert_object_args(&r.calls[0], "tool/parameters");

   cJSON *args = cJSON_Parse(r.calls[0].arguments);
   const cJSON *command = cJSON_GetObjectItemCaseSensitive(args, "command");
   assert(cJSON_IsString(command) && strcmp(command->valuestring, "ls -la") == 0);
   cJSON_Delete(args);
   agent_free_parsed_response(&r);

   /* The established spellings keep working. */
   memset(&r, 0, sizeof(r));
   assert(delegate_rescue_parse_tool_calls("{\"name\": \"bash\", \"args\": {\"command\": \"x\"}}",
                                           &r, 1) == 1);
   assert(strstr(r.calls[0].arguments, "\"x\"") != NULL);
   agent_free_parsed_response(&r);
   printf("  tool/parameters spelling: ok\n");
}

/* ---- 13. reasoning blocks and content extraction ---- */
static void test_reasoning_and_content(void)
{
   parsed_response_t r;
   memset(&r, 0, sizeof(r));
   /* <think> is stripped before parsing, so a call after it is still found and
    * the reasoning does not leak into content. */
   int n = xml_parse_tool_calls("<think>I should list files</think>"
                                "<tool_call><name>bash</name><arguments>{\"command\":\"ls\"}"
                                "</arguments></tool_call>",
                                &r);
   assert(n == 1 && r.call_count == 1);
   assert(r.content == NULL || strstr(r.content, "should list files") == NULL);
   agent_free_parsed_response(&r);

   /* Text before the first block becomes content; text between blocks does not
    * resurface as a second content value. */
   memset(&r, 0, sizeof(r));
   n = xml_parse_tool_calls(
       "first<tool_call><name>bash</name><arguments>{\"a\":1}</arguments></tool_call>"
       "middle<tool_call><name>read</name><arguments>{\"b\":2}</arguments></tool_call>",
       &r);
   assert(n == 2 && r.call_count == 2);
   assert(r.content && strcmp(r.content, "first") == 0);
   agent_free_parsed_response(&r);
   printf("  reasoning and content: ok\n");
}

/* ---- 14. the call cap and malformed-block recovery ---- */
static void test_cap_and_malformed(void)
{
   /* More blocks than the cap: the first AGENT_MAX_TOOL_CALLS are taken and the
    * rest dropped, rather than overrunning the fixed array. */
   size_t n = (size_t)AGENT_MAX_TOOL_CALLS + 8;
   size_t cap = n * 96 + 64;
   char *buf = malloc(cap);
   assert(buf);
   buf[0] = '\0';
   for (size_t i = 0; i < n; i++)
      strcat(buf, "<tool_call><name>bash</name><arguments>{\"a\":1}</arguments></tool_call>");
   parsed_response_t r;
   memset(&r, 0, sizeof(r));
   int got = xml_parse_tool_calls(buf, &r);
   assert(got == AGENT_MAX_TOOL_CALLS && r.call_count == AGENT_MAX_TOOL_CALLS);
   agent_free_parsed_response(&r);
   free(buf);

   /* An unterminated block yields nothing rather than reading past it. */
   memset(&r, 0, sizeof(r));
   assert(xml_parse_tool_calls("<tool_call><name>bash</name><arguments>{\"a\":1}", &r) == 0);
   agent_free_parsed_response(&r);
   printf("  cap and malformed: ok\n");
}

/* ---- 15. REGRESSION: a tool call must carry a usable name ----
 * The block path accepted a call whenever the <name> TAG was present, without
 * checking that a name survived trimming. An empty <name></name> therefore
 * produced a call with name "" -- dispatched to nothing, and indistinguishable
 * downstream from a tool that simply does not exist.
 *
 * A name too long for the field was silently truncated to 31 characters, so two
 * distinct long names collapse to the same prefix and a call can be attributed
 * to the WRONG tool. Refusing is the only safe reading. */
static void test_name_must_be_usable(void)
{
   parsed_response_t r;

   memset(&r, 0, sizeof(r));
   assert(xml_parse_tool_calls("<tool_call><name></name><arguments>{\"a\":1}</arguments>"
                               "</tool_call>",
                               &r) == 0);
   assert(r.call_count == 0);
   agent_free_parsed_response(&r);

   memset(&r, 0, sizeof(r));
   assert(xml_parse_tool_calls("<tool_call><name>   </name><arguments>{\"a\":1}</arguments>"
                               "</tool_call>",
                               &r) == 0);
   agent_free_parsed_response(&r);

   /* Longer than parsed_tool_call_t::name can hold: refused, not truncated. */
   {
      char big[256];
      int off = snprintf(big, sizeof big, "<tool_call><name>");
      memset(big + off, 'a', 80);
      snprintf(big + off + 80, sizeof(big) - (size_t)off - 80,
               "</name><arguments>{\"a\":1}</arguments></tool_call>");
      memset(&r, 0, sizeof(r));
      assert(xml_parse_tool_calls(big, &r) == 0);
      agent_free_parsed_response(&r);
   }

   /* A name that merely needs trimming is still accepted. */
   memset(&r, 0, sizeof(r));
   assert(xml_parse_tool_calls("<tool_call><name>  bash  </name><arguments>{\"a\":1}</arguments>"
                               "</tool_call>",
                               &r) == 1);
   assert(strcmp(r.calls[0].name, "bash") == 0);
   agent_free_parsed_response(&r);
   printf("  name must be usable: ok\n");
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
   test_invoke_format();
   test_qwen_function_format();
   test_mistral_format();
   test_channel_arguments_are_an_object();
   test_detector_and_parser_agree();
   test_bare_json_rescue();
   test_tool_parameters_spelling();
   test_reasoning_and_content();
   test_cap_and_malformed();
   test_name_must_be_usable();
   printf("All delegate_xml_fallback tests passed.\n");
   return 0;
}
