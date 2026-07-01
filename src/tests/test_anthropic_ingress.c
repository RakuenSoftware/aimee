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
   assert(anthropic_tools_to_responses(t) == NULL);
   cJSON_Delete(t);
   assert(anthropic_tools_to_openai(NULL) == NULL);
   assert(anthropic_tools_to_responses(NULL) == NULL);
   PASS("tools_empty");
}

static void test_tools_responses_mapping(void)
{
   cJSON *t = parse("[{\"name\":\"Read\",\"description\":\"Read a file\","
                    "\"input_schema\":{\"type\":\"object\",\"properties\":{}}}]");
   cJSON *out = anthropic_tools_to_responses(t);
   cJSON *tool = cJSON_GetArrayItem(out, 0);
   assert(strcmp(ostr(tool, "type"), "function") == 0);
   assert(strcmp(ostr(tool, "name"), "Read") == 0);
   assert(strcmp(ostr(tool, "description"), "Read a file") == 0);
   assert(cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(tool, "parameters")));
   assert(cJSON_GetObjectItemCaseSensitive(tool, "function") == NULL);
   cJSON_Delete(out);
   cJSON_Delete(t);
   PASS("tools_responses_mapping");
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

static void test_stream_usage_tap(void)
{
   /* The translator captures the full upstream usage, not just completion: the
    * upstream prompt_tokens overrides the begin-time estimate, and OpenAI prompt
    * caching (prompt_tokens_details.cached_tokens) is surfaced for cost. */
   capture_t cap;
   anthropic_stream_xlate_t *st;
   memset(&cap, 0, sizeof(cap));

   st = anthropic_stream_begin("msg_u1", "minimax", 7 /* estimate */, cap_emit, &cap);
   assert(st != NULL);
   anthropic_stream_feed_openai(st, "{\"choices\":[{\"delta\":{\"content\":\"hi\"}}]}");
   anthropic_stream_feed_openai(st, "{\"choices\":[{\"delta\":{},\"finish_reason\":\"stop\"}],"
                                    "\"usage\":{\"prompt_tokens\":123,\"completion_tokens\":45,"
                                    "\"prompt_tokens_details\":{\"cached_tokens\":40}}}");

   int in_tok = -1, out_tok = -1, cr_tok = -1;
   anthropic_stream_get_usage(st, &in_tok, &out_tok, &cr_tok);
   /* Upstream-reported prompt count wins over the 7-token estimate. */
   assert(in_tok == 123);
   assert(out_tok == 45);
   assert(cr_tok == 40);

   anthropic_stream_finish(st);
   anthropic_stream_free(st);

   /* NULL state / NULL out pointers are tolerated. */
   anthropic_stream_get_usage(NULL, &in_tok, NULL, NULL);
   assert(in_tok == 0);
   PASS("stream_usage_tap");
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

/* Per-request passthrough headers (parity mode): set/get echo, NULL handling,
 * per-request reset (no leak across requests), and bounded/NUL-terminated
 * storage for an oversized beta list. */
static void test_request_headers_passthrough(void)
{
   char big[1200];

   /* default state: empty, never NULL. */
   anthropic_ingress_set_request_headers("", "");
   assert(anthropic_ingress_request_version()[0] == '\0');
   assert(anthropic_ingress_request_beta()[0] == '\0');

   /* echo what was captured. */
   anthropic_ingress_set_request_headers(
       "2023-06-01", "fine-grained-tool-streaming-2025-05-14,extended-cache-ttl");
   assert(strcmp(anthropic_ingress_request_version(), "2023-06-01") == 0);
   assert(strstr(anthropic_ingress_request_beta(), "fine-grained-tool-streaming") != NULL);

   /* NULL is treated as empty (clears), not a crash. */
   anthropic_ingress_set_request_headers(NULL, NULL);
   assert(anthropic_ingress_request_version()[0] == '\0');
   assert(anthropic_ingress_request_beta()[0] == '\0');

   /* per-request reset: a value from one request must not leak into the next. */
   anthropic_ingress_set_request_headers("2023-06-01", "beta-a");
   anthropic_ingress_set_request_headers("", "");
   assert(anthropic_ingress_request_beta()[0] == '\0');

   /* oversized beta list is stored bounded + NUL-terminated, no overflow. */
   memset(big, 'x', sizeof(big));
   big[sizeof(big) - 1] = '\0';
   anthropic_ingress_set_request_headers("2023-06-01", big);
   assert(strlen(anthropic_ingress_request_beta()) < 512);

   anthropic_ingress_set_request_headers("", ""); /* leave clean for later tests */
   PASS("request_headers_passthrough");
}

/* --- emit_message_as_sse (P2c streaming replay) ------------------------ */

/* Captured SSE events, in order. The replay helper emits one entry per
 * call to the emit callback; tests assert on (event, payload) pairs. */
#define REPLAY_CAP 32
typedef struct
{
   char events[REPLAY_CAP][32];
   char data[REPLAY_CAP][1024];
   int count;
} replay_capture_t;

static void replay_capture(void *ctx, const char *event, const char *data)
{
   replay_capture_t *c = (replay_capture_t *)ctx;
   assert(c->count < REPLAY_CAP);
   snprintf(c->events[c->count], sizeof(c->events[c->count]), "%s", event);
   snprintf(c->data[c->count], sizeof(c->data[c->count]), "%s", data);
   c->count++;
}

static parsed_response_t seed_parsed_for_replay(int n_calls, const char *names[], const char *ids[],
                                                const char *stop_reason, const char *content)
{
   parsed_response_t p;
   int i;
   memset(&p, 0, sizeof(p));
   p.call_count = n_calls;
   if (stop_reason)
      snprintf(p.stop_reason, sizeof(p.stop_reason), "%s", stop_reason);
   p.content = content ? strdup(content) : NULL;
   p.prompt_tokens = 17;
   p.completion_tokens = 23;
   p.cache_write_tokens = 0;
   p.cache_read_tokens = 0;
   for (i = 0; i < n_calls; i++)
   {
      snprintf(p.calls[i].id, sizeof(p.calls[i].id), "%s", ids[i]);
      snprintf(p.calls[i].name, sizeof(p.calls[i].name), "%s", names[i]);
      p.calls[i].arguments = strdup("{\"k\":\"v\"}");
   }
   return p;
}

static void free_parsed_for_replay(parsed_response_t *p)
{
   int i;
   free(p->content);
   for (i = 0; i < p->call_count; i++)
      free(p->calls[i].arguments);
}

/* Text-only reply: message_start, one text content_block_start / delta /
 * stop, message_delta, message_stop. */
static void test_emit_message_as_sse_replays_text_only(void)
{
   replay_capture_t cap;
   parsed_response_t p = seed_parsed_for_replay(0, NULL, NULL, "end_turn", "hello");
   memset(&cap, 0, sizeof(cap));
   emit_message_as_sse(&p, "msg_t1", "claude-test", replay_capture, &cap);
   assert(cap.count == 6);
   assert(strcmp(cap.events[0], "message_start") == 0);
   assert(strcmp(cap.events[1], "content_block_start") == 0);
   assert(strcmp(cap.events[2], "content_block_delta") == 0);
   assert(strcmp(cap.events[3], "content_block_stop") == 0);
   assert(strcmp(cap.events[4], "message_delta") == 0);
   assert(strcmp(cap.events[5], "message_stop") == 0);
   /* The text_delta payload carries the content string. */
   assert(strstr(cap.data[2], "\"text_delta\"") != NULL);
   assert(strstr(cap.data[2], "\"hello\"") != NULL);
   /* message_delta carries stop_reason verbatim from parsed (end_turn). */
   assert(strstr(cap.data[4], "\"stop_reason\":\"end_turn\"") != NULL);
   /* message_start carries input_tokens = 17, output_tokens not yet (it's
    * in message_delta). */
   assert(strstr(cap.data[0], "\"input_tokens\":17") != NULL);
   free_parsed_for_replay(&p);
   PASS("emit_message_as_sse_replays_text_only");
}

/* Empty content (the "no_text" + "calls_0" combo): one empty text block
 * is emitted, matching the buffered renderer's empty-text-block fallback. */
static void test_emit_message_as_sse_replays_empty_content_as_single_text_block(void)
{
   replay_capture_t cap;
   parsed_response_t p = seed_parsed_for_replay(0, NULL, NULL, "end_turn", NULL);
   memset(&cap, 0, sizeof(cap));
   emit_message_as_sse(&p, "msg_e1", "claude-test", replay_capture, &cap);
   assert(cap.count == 5); /* start, text_start, text_stop, message_delta, message_stop */
   assert(strcmp(cap.events[0], "message_start") == 0);
   assert(strcmp(cap.events[1], "content_block_start") == 0);
   assert(strcmp(cap.events[2], "content_block_stop") == 0);
   assert(strcmp(cap.events[3], "message_delta") == 0);
   assert(strcmp(cap.events[4], "message_stop") == 0);
   free_parsed_for_replay(&p);
   PASS("emit_message_as_sse_replays_empty_content_as_single_text_block");
}

/* Single surviving tool_use (post-police): message_start, empty text
 * block, tool_use content_block_start + input_json_delta + stop,
 * message_delta, message_stop. */
static void test_emit_message_as_sse_replays_surviving_tool_calls(void)
{
   replay_capture_t cap;
   const char *names[] = {"web_search"};
   const char *ids[] = {"toolu_test_1"};
   parsed_response_t p = seed_parsed_for_replay(1, names, ids, "tool_use", NULL);
   memset(&cap, 0, sizeof(cap));
   emit_message_as_sse(&p, "msg_s1", "claude-test", replay_capture, &cap);
   /* start, text_start, text_stop, tool_start, input_delta, tool_stop, md, ms = 8 */
   assert(cap.count == 8);
   assert(strcmp(cap.events[3], "content_block_start") == 0); /* tool_use */
   assert(strstr(cap.data[3], "\"type\":\"tool_use\"") != NULL);
   assert(strstr(cap.data[3], "\"name\":\"web_search\"") != NULL);
   assert(strstr(cap.data[3], "\"index\":1") != NULL);        /* index 1 (after text) */
   assert(strcmp(cap.events[4], "content_block_delta") == 0); /* input_json_delta */
   assert(strstr(cap.data[4], "\"input_json_delta\"") != NULL);
   free_parsed_for_replay(&p);
   PASS("emit_message_as_sse_replays_surviving_tool_calls");
}

/* Text + surviving tool_use in the same reply: text block index 0,
 * tool_use block index 1. */
static void test_emit_message_as_sse_replays_text_and_tool_calls(void)
{
   replay_capture_t cap;
   const char *names[] = {"web_search"};
   const char *ids[] = {"toolu_1"};
   parsed_response_t p = seed_parsed_for_replay(1, names, ids, "tool_use", "context");
   memset(&cap, 0, sizeof(cap));
   emit_message_as_sse(&p, "msg_b1", "claude-test", replay_capture, &cap);
   /* start, text_start, text_delta, text_stop, tool_start, input_delta,
    * tool_stop, md, ms = 9 */
   assert(cap.count == 9);
   assert(strcmp(cap.events[1], "content_block_start") == 0);
   assert(strstr(cap.data[1], "\"index\":0") != NULL);
   assert(strstr(cap.data[1], "\"type\":\"text\"") != NULL);
   assert(strcmp(cap.events[4], "content_block_start") == 0);
   assert(strstr(cap.data[4], "\"index\":1") != NULL);
   assert(strstr(cap.data[4], "\"type\":\"tool_use\"") != NULL);
   assert(strstr(cap.data[4], "\"name\":\"web_search\"") != NULL);
   free_parsed_for_replay(&p);
   PASS("emit_message_as_sse_replays_text_and_tool_calls");
}

/* Multiple surviving tool_use blocks: police has already compacted to
 * zero or more; verify per-block indices are correct (0, 1, 2...) and the
 * original names are preserved. */
static void test_emit_message_as_sse_replays_multiple_surviving_tool_calls(void)
{
   replay_capture_t cap;
   const char *names[] = {"web_search", "Read"}; /* police already dropped the middle one */
   const char *ids[] = {"t1", "t3"};
   parsed_response_t p = seed_parsed_for_replay(2, names, ids, "tool_use", NULL);
   memset(&cap, 0, sizeof(cap));
   emit_message_as_sse(&p, "msg_m1", "claude-test", replay_capture, &cap);
   /* start, text_start, text_stop, [tool_start, input_delta, tool_stop] x2,
    * md, ms = 1 + 2 + 6 + 1 + 1 = 11 */
   assert(cap.count == 11);
   /* Two tool_use content_block_start events at indices 3 and 6 (events 3, 6). */
   assert(strstr(cap.data[3], "\"index\":1") != NULL); /* first tool, index 1 */
   assert(strstr(cap.data[3], "\"name\":\"web_search\"") != NULL);
   assert(strstr(cap.data[6], "\"index\":2") != NULL); /* second tool, index 2 */
   assert(strstr(cap.data[6], "\"name\":\"Read\"") != NULL);
   free_parsed_for_replay(&p);
   PASS("emit_message_as_sse_replays_multiple_surviving_tool_calls");
}

/* Cache tokens propagate: cache_creation in message_start.usage (input
 * side) and cache_read in message_delta.usage (output side). Distinct
 * fields per Anthropic's wire format. */
static void test_emit_message_as_sse_propagates_usage(void)
{
   replay_capture_t cap;
   parsed_response_t p = seed_parsed_for_replay(0, NULL, NULL, "end_turn", "hi");
   p.prompt_tokens = 100;
   p.completion_tokens = 50;
   p.cache_write_tokens = 25; /* Anthropic: cache_creation_input_tokens */
   p.cache_read_tokens = 10;
   memset(&cap, 0, sizeof(cap));
   emit_message_as_sse(&p, "msg_u1", "claude-test", replay_capture, &cap);
   /* message_start.usage carries input_tokens + cache_creation + cache_read. */
   assert(strstr(cap.data[0], "\"input_tokens\":100") != NULL);
   assert(strstr(cap.data[0], "\"cache_creation_input_tokens\":25") != NULL);
   assert(strstr(cap.data[0], "\"cache_read_input_tokens\":10") != NULL);
   /* message_delta.usage carries output_tokens + cache_read. */
   assert(strstr(cap.data[4], "\"output_tokens\":50") != NULL);
   assert(strstr(cap.data[4], "\"cache_read_input_tokens\":10") != NULL);
   free_parsed_for_replay(&p);
   PASS("emit_message_as_sse_propagates_usage");
}

/* Pinned B2 fix: upstream `max_tokens` with surviving non-subagent calls
 * → wire carries "max_tokens" (NOT "end_turn", NOT "tool_use" — the
 * partial-drop preservation rule). The all-dropped case rewrites to
 * "end_turn" (covered separately by the police function's own unit
 * test); this test pins the partial-drop replay shape. */
static void test_emit_message_as_sse_preserves_max_tokens(void)
{
   replay_capture_t cap;
   const char *names[] = {"Task", "web_search"};
   const char *ids[] = {"t1", "t2"};
   parsed_response_t p = seed_parsed_for_replay(2, names, ids, "max_tokens", "");
   memset(&cap, 0, sizeof(cap));
   /* After the police function runs (the streaming path runs it before
    * calling us), only web_search survives; the upstream's max_tokens is
    * preserved verbatim. We simulate the post-police state directly. */
   free_parsed_for_replay(&p);
   const char *names2[] = {"web_search"};
   const char *ids2[] = {"t2"};
   p = seed_parsed_for_replay(1, names2, ids2, "max_tokens", "");
   memset(&cap, 0, sizeof(cap));
   emit_message_as_sse(&p, "msg_mt", "claude-test", replay_capture, &cap);
   /* The replay must carry stop_reason="max_tokens" verbatim — NOT
    * re-derive "tool_use" from call_count > 0. */
   for (int i = 0; i < cap.count; i++)
   {
      if (strcmp(cap.events[i], "message_delta") == 0)
         assert(strstr(cap.data[i], "\"stop_reason\":\"max_tokens\"") != NULL);
   }
   free_parsed_for_replay(&p);
   PASS("emit_message_as_sse_preserves_max_tokens");
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
   test_tools_responses_mapping();
   test_response_text();
   test_response_tool_use();
   test_response_empty_gets_text_block();
   test_stream_text();
   test_stream_usage_tap();
   test_stream_tool_call();
   test_stream_text_then_tool();
   test_request_headers_passthrough();
   test_emit_message_as_sse_replays_text_only();
   test_emit_message_as_sse_replays_empty_content_as_single_text_block();
   test_emit_message_as_sse_replays_surviving_tool_calls();
   test_emit_message_as_sse_replays_text_and_tool_calls();
   test_emit_message_as_sse_replays_multiple_surviving_tool_calls();
   test_emit_message_as_sse_propagates_usage();
   test_emit_message_as_sse_preserves_max_tokens();
   printf("all anthropic_ingress tests passed\n");
   return 0;
}
