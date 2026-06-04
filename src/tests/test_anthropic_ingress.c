/* test_anthropic_ingress.c: unit tests for the inbound Anthropic Messages API
 * translation (Anthropic Messages JSON <-> OpenAI chat/completions JSON, plus
 * parsed-reply -> Anthropic response shaping). Pure cJSON; no I/O. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../headers/anthropic_ingress.h"
#include "../vendor/headers/cJSON.h"

#define PASS(name) printf("  PASS: %s\n", (name))

static cJSON *parse(const char *json)
{
   cJSON *j = cJSON_Parse(json);
   assert(j != NULL);
   return j;
}

static const char *ostr(const cJSON *obj, const char *key)
{
   const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
   return (cJSON_IsString(v)) ? v->valuestring : NULL;
}

/* ------------------------------------------------------------------ system */

static void test_system_string(void)
{
   cJSON *req = parse("{\"system\":\"You are helpful.\"}");
   char *s = anthropic_system_to_text(req);
   assert(s && strcmp(s, "You are helpful.") == 0);
   free(s);
   cJSON_Delete(req);
   PASS("system_string");
}

static void test_system_array(void)
{
   cJSON *req = parse("{\"system\":[{\"type\":\"text\",\"text\":\"A\"},"
                      "{\"type\":\"text\",\"text\":\"B\"}]}");
   char *s = anthropic_system_to_text(req);
   assert(s && strcmp(s, "A\n\nB") == 0);
   free(s);
   cJSON_Delete(req);
   PASS("system_array");
}

static void test_system_absent(void)
{
   cJSON *req = parse("{\"messages\":[]}");
   char *s = anthropic_system_to_text(req);
   assert(s == NULL);
   cJSON_Delete(req);
   PASS("system_absent");
}

/* ---------------------------------------------------------------- messages */

static void test_messages_simple_string(void)
{
   cJSON *m = parse("[{\"role\":\"user\",\"content\":\"hello\"}]");
   cJSON *out = anthropic_messages_to_openai(m, "SYS");
   assert(cJSON_GetArraySize(out) == 2);
   assert(strcmp(ostr(cJSON_GetArrayItem(out, 0), "role"), "system") == 0);
   assert(strcmp(ostr(cJSON_GetArrayItem(out, 0), "content"), "SYS") == 0);
   assert(strcmp(ostr(cJSON_GetArrayItem(out, 1), "role"), "user") == 0);
   assert(strcmp(ostr(cJSON_GetArrayItem(out, 1), "content"), "hello") == 0);
   cJSON_Delete(out);
   cJSON_Delete(m);
   PASS("messages_simple_string");
}

static void test_messages_assistant_tool_use(void)
{
   cJSON *m = parse("[{\"role\":\"assistant\",\"content\":["
                    "{\"type\":\"text\",\"text\":\"reading\"},"
                    "{\"type\":\"tool_use\",\"id\":\"toolu_1\",\"name\":\"Read\","
                    "\"input\":{\"path\":\"a.c\"}}]}]");
   cJSON *out = anthropic_messages_to_openai(m, NULL);
   cJSON *asst, *calls, *call, *fn, *args;

   assert(cJSON_GetArraySize(out) == 1);
   asst = cJSON_GetArrayItem(out, 0);
   assert(strcmp(ostr(asst, "role"), "assistant") == 0);
   assert(strcmp(ostr(asst, "content"), "reading") == 0);

   calls = cJSON_GetObjectItemCaseSensitive(asst, "tool_calls");
   assert(cJSON_IsArray(calls) && cJSON_GetArraySize(calls) == 1);
   call = cJSON_GetArrayItem(calls, 0);
   assert(strcmp(ostr(call, "id"), "toolu_1") == 0);
   assert(strcmp(ostr(call, "type"), "function") == 0);
   fn = cJSON_GetObjectItemCaseSensitive(call, "function");
   assert(strcmp(ostr(fn, "name"), "Read") == 0);

   /* arguments must be a JSON *string*; round-trip it. */
   args = parse(ostr(fn, "arguments"));
   assert(strcmp(ostr(args, "path"), "a.c") == 0);
   cJSON_Delete(args);
   cJSON_Delete(out);
   cJSON_Delete(m);
   PASS("messages_assistant_tool_use");
}

static void test_messages_tool_use_no_text(void)
{
   /* tool_use with no text -> assistant content must be JSON null. */
   cJSON *m = parse("[{\"role\":\"assistant\",\"content\":["
                    "{\"type\":\"tool_use\",\"id\":\"t\",\"name\":\"X\",\"input\":{}}]}]");
   cJSON *out = anthropic_messages_to_openai(m, NULL);
   cJSON *asst = cJSON_GetArrayItem(out, 0);
   cJSON *content = cJSON_GetObjectItemCaseSensitive(asst, "content");
   assert(cJSON_IsNull(content));
   assert(cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(asst, "tool_calls")) == 1);
   cJSON_Delete(out);
   cJSON_Delete(m);
   PASS("messages_tool_use_no_text");
}

static void test_messages_user_tool_result(void)
{
   /* tool_result must become a role:tool message, emitted before user text. */
   cJSON *m = parse("[{\"role\":\"user\",\"content\":["
                    "{\"type\":\"tool_result\",\"tool_use_id\":\"toolu_1\","
                    "\"content\":\"file body\"},"
                    "{\"type\":\"text\",\"text\":\"thanks\"}]}]");
   cJSON *out = anthropic_messages_to_openai(m, NULL);
   cJSON *tool, *usr;

   assert(cJSON_GetArraySize(out) == 2);
   tool = cJSON_GetArrayItem(out, 0);
   assert(strcmp(ostr(tool, "role"), "tool") == 0);
   assert(strcmp(ostr(tool, "tool_call_id"), "toolu_1") == 0);
   assert(strcmp(ostr(tool, "content"), "file body") == 0);

   usr = cJSON_GetArrayItem(out, 1);
   assert(strcmp(ostr(usr, "role"), "user") == 0);
   assert(strcmp(ostr(usr, "content"), "thanks") == 0);
   cJSON_Delete(out);
   cJSON_Delete(m);
   PASS("messages_user_tool_result");
}

static void test_messages_image(void)
{
   cJSON *m = parse("[{\"role\":\"user\",\"content\":["
                    "{\"type\":\"text\",\"text\":\"what is this\"},"
                    "{\"type\":\"image\",\"source\":{\"type\":\"base64\","
                    "\"media_type\":\"image/png\",\"data\":\"AAAA\"}}]}]");
   cJSON *out = anthropic_messages_to_openai(m, NULL);
   cJSON *usr, *parts, *p0, *p1, *iu;

   assert(cJSON_GetArraySize(out) == 1);
   usr = cJSON_GetArrayItem(out, 0);
   parts = cJSON_GetObjectItemCaseSensitive(usr, "content");
   assert(cJSON_IsArray(parts) && cJSON_GetArraySize(parts) == 2);
   p0 = cJSON_GetArrayItem(parts, 0);
   assert(strcmp(ostr(p0, "type"), "text") == 0);
   p1 = cJSON_GetArrayItem(parts, 1);
   assert(strcmp(ostr(p1, "type"), "image_url") == 0);
   iu = cJSON_GetObjectItemCaseSensitive(p1, "image_url");
   assert(strcmp(ostr(iu, "url"), "data:image/png;base64,AAAA") == 0);
   cJSON_Delete(out);
   cJSON_Delete(m);
   PASS("messages_image");
}

/* ------------------------------------------------------------------- tools */

static void test_tools_mapping(void)
{
   cJSON *t = parse("[{\"name\":\"Read\",\"description\":\"Read a file\","
                    "\"input_schema\":{\"type\":\"object\","
                    "\"properties\":{\"path\":{\"type\":\"string\"}},"
                    "\"required\":[\"path\"]}}]");
   cJSON *out = anthropic_tools_to_openai(t);
   cJSON *w, *fn, *params, *props;

   assert(out && cJSON_GetArraySize(out) == 1);
   w = cJSON_GetArrayItem(out, 0);
   assert(strcmp(ostr(w, "type"), "function") == 0);
   fn = cJSON_GetObjectItemCaseSensitive(w, "function");
   assert(strcmp(ostr(fn, "name"), "Read") == 0);
   assert(strcmp(ostr(fn, "description"), "Read a file") == 0);
   params = cJSON_GetObjectItemCaseSensitive(fn, "parameters");
   assert(strcmp(ostr(params, "type"), "object") == 0);
   props = cJSON_GetObjectItemCaseSensitive(params, "properties");
   assert(cJSON_GetObjectItemCaseSensitive(props, "path") != NULL);
   cJSON_Delete(out);
   cJSON_Delete(t);
   PASS("tools_mapping");
}

static void test_tools_empty(void)
{
   cJSON *t = parse("[]");
   assert(anthropic_tools_to_openai(t) == NULL);
   cJSON_Delete(t);
   assert(anthropic_tools_to_openai(NULL) == NULL);
   PASS("tools_empty");
}

/* --------------------------------------------------------------- responses */

static void test_response_text(void)
{
   parsed_response_t p;
   cJSON *r, *content, *tb, *usage;

   memset(&p, 0, sizeof(p));
   p.content = (char *)"Hi there";
   p.call_count = 0;
   p.prompt_tokens = 10;
   p.completion_tokens = 5;

   r = anthropic_response_from_parsed("msg_1", "claude-opus-4", &p);
   assert(strcmp(ostr(r, "id"), "msg_1") == 0);
   assert(strcmp(ostr(r, "type"), "message") == 0);
   assert(strcmp(ostr(r, "role"), "assistant") == 0);
   assert(strcmp(ostr(r, "model"), "claude-opus-4") == 0);
   assert(strcmp(ostr(r, "stop_reason"), "end_turn") == 0);

   content = cJSON_GetObjectItemCaseSensitive(r, "content");
   assert(cJSON_GetArraySize(content) == 1);
   tb = cJSON_GetArrayItem(content, 0);
   assert(strcmp(ostr(tb, "type"), "text") == 0);
   assert(strcmp(ostr(tb, "text"), "Hi there") == 0);

   usage = cJSON_GetObjectItemCaseSensitive(r, "usage");
   assert(cJSON_GetObjectItemCaseSensitive(usage, "input_tokens")->valueint == 10);
   assert(cJSON_GetObjectItemCaseSensitive(usage, "output_tokens")->valueint == 5);
   cJSON_Delete(r);
   PASS("response_text");
}

static void test_response_tool_use(void)
{
   parsed_response_t p;
   cJSON *r, *content, *ub, *input;

   memset(&p, 0, sizeof(p));
   p.content = (char *)"";
   p.is_tool_call = 1;
   p.call_count = 1;
   strcpy(p.calls[0].id, "toolu_x");
   strcpy(p.calls[0].name, "Bash");
   p.calls[0].arguments = (char *)"{\"cmd\":\"ls\"}";

   r = anthropic_response_from_parsed("msg_2", "minimax", &p);
   assert(strcmp(ostr(r, "stop_reason"), "tool_use") == 0);

   content = cJSON_GetObjectItemCaseSensitive(r, "content");
   assert(cJSON_GetArraySize(content) == 1);
   ub = cJSON_GetArrayItem(content, 0);
   assert(strcmp(ostr(ub, "type"), "tool_use") == 0);
   assert(strcmp(ostr(ub, "id"), "toolu_x") == 0);
   assert(strcmp(ostr(ub, "name"), "Bash") == 0);
   input = cJSON_GetObjectItemCaseSensitive(ub, "input");
   assert(cJSON_IsObject(input));
   assert(strcmp(ostr(input, "cmd"), "ls") == 0);
   cJSON_Delete(r);
   PASS("response_tool_use");
}

static void test_response_empty_gets_text_block(void)
{
   parsed_response_t p;
   cJSON *r, *content;

   memset(&p, 0, sizeof(p));
   p.content = (char *)"";
   p.call_count = 0;

   r = anthropic_response_from_parsed(NULL, NULL, &p);
   /* defaults applied for id/model */
   assert(strcmp(ostr(r, "id"), "msg_aimee") == 0);
   assert(strcmp(ostr(r, "model"), "aimee-primary") == 0);
   content = cJSON_GetObjectItemCaseSensitive(r, "content");
   assert(cJSON_GetArraySize(content) == 1);
   assert(strcmp(ostr(cJSON_GetArrayItem(content, 0), "type"), "text") == 0);
   cJSON_Delete(r);
   PASS("response_empty_gets_text_block");
}

/* --------------------------------------------------------------- streaming */

/* Capture emitted events as "event\n" tokens plus the full data payloads, so a
 * test can assert both ordering and content. */
typedef struct
{
   char events[4096]; /* newline-separated event names, in order */
   char data[8192];   /* concatenated data payloads */
} capture_t;

static void cap_emit(void *ctx, const char *event, const char *data_json)
{
   capture_t *c = (capture_t *)ctx;
   strncat(c->events, event, sizeof(c->events) - strlen(c->events) - 1);
   strncat(c->events, "\n", sizeof(c->events) - strlen(c->events) - 1);
   strncat(c->data, data_json, sizeof(c->data) - strlen(c->data) - 1);
   strncat(c->data, "\n", sizeof(c->data) - strlen(c->data) - 1);
}

static void test_stream_text(void)
{
   capture_t cap;
   anthropic_stream_xlate_t *st;
   memset(&cap, 0, sizeof(cap));

   st = anthropic_stream_begin("msg_s1", "minimax", 7, cap_emit, &cap);
   assert(st != NULL);
   anthropic_stream_feed_openai(st, "{\"choices\":[{\"delta\":{\"content\":\"Hel\"}}]}");
   anthropic_stream_feed_openai(st, "{\"choices\":[{\"delta\":{\"content\":\"lo\"}}]}");
   anthropic_stream_feed_openai(st, "{\"choices\":[{\"delta\":{},\"finish_reason\":\"stop\"}],"
                                    "\"usage\":{\"completion_tokens\":4}}");
   anthropic_stream_finish(st);
   anthropic_stream_free(st);

   /* Event order: start, block_start, 2 deltas, block_stop, msg_delta, stop. */
   assert(strcmp(cap.events, "message_start\n"
                             "content_block_start\n"
                             "content_block_delta\n"
                             "content_block_delta\n"
                             "content_block_stop\n"
                             "message_delta\n"
                             "message_stop\n") == 0);
   assert(strstr(cap.data, "\"text\":\"Hel\"") != NULL);
   assert(strstr(cap.data, "\"text\":\"lo\"") != NULL);
   assert(strstr(cap.data, "\"stop_reason\":\"end_turn\"") != NULL);
   assert(strstr(cap.data, "\"output_tokens\":4") != NULL);
   assert(strstr(cap.data, "\"input_tokens\":7") != NULL);
   PASS("stream_text");
}

static void test_stream_tool_call(void)
{
   capture_t cap;
   anthropic_stream_xlate_t *st;
   memset(&cap, 0, sizeof(cap));

   st = anthropic_stream_begin("msg_s2", "minimax", 0, cap_emit, &cap);
   /* opening fragment carries id + name, empty args */
   anthropic_stream_feed_openai(st, "{\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,"
                                    "\"id\":\"call_1\",\"function\":{\"name\":\"Bash\","
                                    "\"arguments\":\"\"}}]}}]}");
   anthropic_stream_feed_openai(st, "{\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,"
                                    "\"function\":{\"arguments\":\"{\\\"cmd\\\":\"}}]}}]}");
   anthropic_stream_feed_openai(st, "{\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,"
                                    "\"function\":{\"arguments\":\"\\\"ls\\\"}\"}}]}}]}");
   anthropic_stream_feed_openai(st, "{\"choices\":[{\"delta\":{},"
                                    "\"finish_reason\":\"tool_calls\"}]}");
   anthropic_stream_finish(st);
   anthropic_stream_free(st);

   assert(strcmp(cap.events, "message_start\n"
                             "content_block_start\n"
                             "content_block_delta\n"
                             "content_block_delta\n"
                             "content_block_stop\n"
                             "message_delta\n"
                             "message_stop\n") == 0);
   assert(strstr(cap.data, "\"type\":\"tool_use\"") != NULL);
   assert(strstr(cap.data, "\"name\":\"Bash\"") != NULL);
   assert(strstr(cap.data, "\"id\":\"call_1\"") != NULL);
   assert(strstr(cap.data, "\"partial_json\":\"{\\\"cmd\\\":\"") != NULL);
   assert(strstr(cap.data, "\"partial_json\":\"\\\"ls\\\"}\"") != NULL);
   assert(strstr(cap.data, "\"stop_reason\":\"tool_use\"") != NULL);
   PASS("stream_tool_call");
}

static void test_stream_text_then_tool(void)
{
   /* text first, then a tool call -> text block closes before tool block. */
   capture_t cap;
   anthropic_stream_xlate_t *st;
   memset(&cap, 0, sizeof(cap));

   st = anthropic_stream_begin("msg_s3", "minimax", 0, cap_emit, &cap);
   anthropic_stream_feed_openai(st, "{\"choices\":[{\"delta\":{\"content\":\"thinking\"}}]}");
   anthropic_stream_feed_openai(st, "{\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,"
                                    "\"id\":\"c1\",\"function\":{\"name\":\"X\","
                                    "\"arguments\":\"{}\"}}]}}]}");
   anthropic_stream_finish(st);
   anthropic_stream_free(st);

   /* text block (index 0) fully closed before tool block (index 1) opens. */
   assert(strcmp(cap.events, "message_start\n"
                             "content_block_start\n" /* text idx0 */
                             "content_block_delta\n" /* text */
                             "content_block_stop\n"  /* close text */
                             "content_block_start\n" /* tool idx1 */
                             "content_block_delta\n" /* json */
                             "content_block_stop\n"  /* close tool (finish) */
                             "message_delta\n"
                             "message_stop\n") == 0);
   assert(strstr(cap.data, "\"index\":0") != NULL);
   assert(strstr(cap.data, "\"index\":1") != NULL);
   PASS("stream_text_then_tool");
}

int main(void)
{
   printf("test_anthropic_ingress:\n");
   test_system_string();
   test_system_array();
   test_system_absent();
   test_messages_simple_string();
   test_messages_assistant_tool_use();
   test_messages_tool_use_no_text();
   test_messages_user_tool_result();
   test_messages_image();
   test_tools_mapping();
   test_tools_empty();
   test_response_text();
   test_response_tool_use();
   test_response_empty_gets_text_block();
   test_stream_text();
   test_stream_tool_call();
   test_stream_text_then_tool();
   printf("all anthropic_ingress tests passed\n");
   return 0;
}
