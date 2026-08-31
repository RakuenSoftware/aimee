/* test_openai_chat_policed.c: unit tests for the P2c response-side
 * tool-policing OpenAI /v1/responses SSE replay helper
 * (openai_responses_emit_policed). Pure shape tests — no agent
 * execution, no real provider, no streaming transport; just the
 * post-police `parsed_response_t` -> SSE event tail.
 *
 * The helper emits ONLY the tool-call tail; the leading
 * `response.created` envelope event is the caller's responsibility
 * (emitted once per response for every path). These tests guard that
 * contract — emitting created here too would double it on the wire.
 *
 * Scope: these tests exercise the helper's consumption of an already
 * governed parsed_response_t (calls[] front-packed, call_count lowered).
 * The event-bus decision seam is tested separately in
 * test_response_governance_stage.c; seeding the governed shape directly here
 * keeps this test focused on wire rendering. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "openai_shape.h"
/* aimee.h before agent_protocol.h: agent_types.h (pulled by agent_protocol.h)
 * needs the MAX_PATH_LEN size macro from aimee.h. */
#include "aimee.h"
#include "agent_protocol.h"
#include "cJSON.h"

#define PASS(name) printf("  PASS: %s\n", (name))

/* Captured SSE events. Sized to the worst case these tests exercise (9
 * events; payloads well under 2KB) to keep the stack object small. */
#define REPLAY_CAP 16
typedef struct
{
   char events[REPLAY_CAP][64];
   char data[REPLAY_CAP][2048];
   int count;
} cap_t;

static void cap_emit(void *ctx, const char *event, const char *data)
{
   cap_t *c = (cap_t *)ctx;
   assert(c->count < REPLAY_CAP);
   snprintf(c->events[c->count], sizeof(c->events[c->count]), "%s", event);
   snprintf(c->data[c->count], sizeof(c->data[c->count]), "%s", data);
   c->count++;
}

/* Build a parsed_response_t with N tool calls (caller-supplied names +
 * ids + arguments). is_tool_call=1 so the dispatcher takes the
 * tool-call branch. */
static void seed_parsed(parsed_response_t *p, int n, const char *names[], const char *ids[],
                        const char *args[], const char *stop_reason)
{
   int i;
   memset(p, 0, sizeof(*p));
   p->call_count = n;
   p->is_tool_call = 1;
   if (stop_reason)
      snprintf(p->stop_reason, sizeof(p->stop_reason), "%s", stop_reason);
   p->prompt_tokens = 17;
   p->completion_tokens = 23;
   for (i = 0; i < n; i++)
   {
      snprintf(p->calls[i].id, sizeof(p->calls[i].id), "%s", ids[i]);
      snprintf(p->calls[i].name, sizeof(p->calls[i].name), "%s", names[i]);
      p->calls[i].arguments = strdup(args[i] ? args[i] : "{}");
   }
}

static void free_parsed(parsed_response_t *p)
{
   int i;
   for (i = 0; i < p->call_count; i++)
      free(p->calls[i].arguments);
}

/* Defense-in-depth: assert no captured event is `response.created` — the
 * helper must never emit the envelope on ANY path (the caller owns it). */
static void assert_no_created(const cap_t *c)
{
   for (int i = 0; i < c->count; i++)
      assert(strcmp(c->events[i], "response.created") != 0);
}

/* --- tests --- */

/* Regression guard: the helper must NEVER emit `response.created` — the
 * caller (responses_stream_handler) already emits it once for every path.
 * A helper that re-emitted it would double `response.created` on the wire,
 * which OpenAI/Codex clients treat as a protocol violation. */
static void test_emit_omits_created_envelope(void)
{
   cap_t cap = {0};
   const char *names[] = {"web_search"};
   const char *ids[] = {"call_1"};
   const char *args[] = {"{\"query\":\"aimee\"}"};
   parsed_response_t p;
   seed_parsed(&p, 1, names, ids, args, "tool_calls");
   openai_responses_emit_policed(&p, "resp_1", "claude-test", 12345L, cap_emit, &cap);
   assert_no_created(&cap);
   /* First event is the tool-call item, not the envelope. */
   assert(cap.count >= 1);
   assert(strcmp(cap.events[0], "response.output_item.added") == 0);
   free_parsed(&p);
   PASS("emit_omits_created_envelope");
}

/* Single surviving tool_call: per-call frames + completed (no created). */
static void test_emit_surviving_tool_call(void)
{
   cap_t cap = {0};
   const char *names[] = {"web_search"};
   const char *ids[] = {"call_1"};
   const char *args[] = {"{\"query\":\"aimee\"}"};
   parsed_response_t p;
   seed_parsed(&p, 1, names, ids, args, "tool_calls");
   openai_responses_emit_policed(&p, "resp_1", "claude-test", 12345L, cap_emit, &cap);

   /* (output_item.added, args.delta, args.done, output_item.done)
    * + response.completed = 5 events. */
   assert(cap.count == 5);
   assert(strcmp(cap.events[0], "response.output_item.added") == 0);
   assert(strstr(cap.data[0], "\"name\":\"web_search\"") != NULL);
   assert(strstr(cap.data[0], "\"call_id\":\"call_1\"") != NULL);
   assert(strcmp(cap.events[1], "response.function_call_arguments.delta") == 0);
   assert(strstr(cap.data[1], "aimee") != NULL);
   assert(strcmp(cap.events[2], "response.function_call_arguments.done") == 0);
   assert(strcmp(cap.events[3], "response.output_item.done") == 0);
   assert(strcmp(cap.events[4], "response.completed") == 0);
   assert(strstr(cap.data[4], "\"output\":[") != NULL);
   free_parsed(&p);
   PASS("emit_surviving_tool_call");
}

/* A namespaced call keeps its group on EVERY frame the client reads.
 *
 * A Codex client offers its MCP tools inside a `namespace` group, and the provider
 * answers with the nested name BARE and the group beside it. The client routes on
 * the pair, so a frame that carries the name without the group is unroutable -- it
 * answers "unsupported call: git". `added` matters as much as `done`, because the
 * client learns the name there, before the arguments stream.
 *
 * `completed` matters too: it replays the output items, and the turn loop feeds
 * those back as the next request's history. Losing the group there would strand
 * the call on the following turn even after the first one routed. */
static void test_emit_namespaced_tool_call(void)
{
   cap_t cap = {0};
   const char *names[] = {"git"};
   const char *ids[] = {"call_7"};
   const char *args[] = {"{\"command\":\"status\"}"};
   parsed_response_t p;
   seed_parsed(&p, 1, names, ids, args, "tool_calls");
   snprintf(p.calls[0].tool_namespace, sizeof(p.calls[0].tool_namespace), "%s", "mcp__aimee");

   openai_responses_emit_policed(&p, "resp_1", "claude-test", 12345L, cap_emit, &cap);

   assert(cap.count == 5);
   /* added, done, and the replayed item inside completed */
   assert(strcmp(cap.events[0], "response.output_item.added") == 0);
   assert(strstr(cap.data[0], "\"name\":\"git\"") != NULL);
   assert(strstr(cap.data[0], "\"namespace\":\"mcp__aimee\"") != NULL);
   assert(strcmp(cap.events[3], "response.output_item.done") == 0);
   assert(strstr(cap.data[3], "\"namespace\":\"mcp__aimee\"") != NULL);
   assert(strcmp(cap.events[4], "response.completed") == 0);
   assert(strstr(cap.data[4], "\"namespace\":\"mcp__aimee\"") != NULL);
   free_parsed(&p);
   PASS("emit_namespaced_tool_call");
}

/* The same path with no group emits no `namespace` key at all -- an empty one
 * would claim a group that does not exist, and every non-Codex client sends
 * ungrouped tools. */
static void test_emit_plain_tool_call_has_no_namespace(void)
{
   cap_t cap = {0};
   const char *names[] = {"web_search"};
   const char *ids[] = {"call_1"};
   const char *args[] = {"{\"query\":\"aimee\"}"};
   parsed_response_t p;
   seed_parsed(&p, 1, names, ids, args, "tool_calls");
   openai_responses_emit_policed(&p, "resp_1", "claude-test", 12345L, cap_emit, &cap);
   for (int i = 0; i < cap.count; i++)
      assert(strstr(cap.data[i], "\"namespace\"") == NULL);
   free_parsed(&p);
   PASS("emit_plain_tool_call_has_no_namespace");
}

/* Two surviving tool_calls: indices in `output[]` are 0 and 1. */
static void test_emit_multiple_surviving_tool_calls(void)
{
   cap_t cap = {0};
   const char *names[] = {"web_search", "Read"};
   const char *ids[] = {"call_1", "call_2"};
   const char *args[] = {"{\"q\":\"x\"}", "{\"p\":\"/etc\"}"};
   parsed_response_t p;
   seed_parsed(&p, 2, names, ids, args, "tool_calls");
   openai_responses_emit_policed(&p, "resp_1", "claude-test", 12345L, cap_emit, &cap);

   /* (4 per-call frames × 2) + response.completed = 9. */
   assert(cap.count == 9);
   /* The two `output_item.added` events at indices 0 and 4 carry
    * fc_id suffixes "-fc-0" and "-fc-1" (built from the loop index). */
   assert(strstr(cap.data[0], "-fc-0") != NULL);
   assert(strstr(cap.data[4], "-fc-1") != NULL);
   /* Names preserved in original order. */
   assert(strstr(cap.data[0], "web_search") != NULL);
   assert(strstr(cap.data[4], "Read") != NULL);
   assert(strcmp(cap.events[8], "response.completed") == 0);
   free_parsed(&p);
   PASS("emit_multiple_surviving_tool_calls");
}

/* All-dropped (non-NULL parsed, call_count == 0): a single empty-output
 * response.completed, no per-call frames and no created. This is the realistic
 * post-police state (police mutated calls[] to empty in place) — distinct from
 * the defensive NULL-parsed case below. */
static void test_emit_all_dropped_empty_output(void)
{
   cap_t cap = {0};
   parsed_response_t p;
   seed_parsed(&p, 0, NULL, NULL, NULL, "end_turn");
   openai_responses_emit_policed(&p, "resp_1", "claude-test", 12345L, cap_emit, &cap);

   assert(cap.count == 1);
   assert_no_created(&cap);
   assert(strcmp(cap.events[0], "response.completed") == 0);
   /* Empty output array — `[]`. */
   assert(strstr(cap.data[0], "\"output\":[]") != NULL);
   /* Usage from parsed is preserved even with no surviving calls. */
   assert(strstr(cap.data[0], "\"input_tokens\":17") != NULL);
   assert(strstr(cap.data[0], "\"output_tokens\":23") != NULL);
   free_parsed(&p);
   PASS("emit_all_dropped_empty_output");
}

/* A call whose arguments pointer is NULL falls back to "{}" on the wire
 * (the `arguments ? arguments : "{}"` guard — arguments is the only
 * heap pointer; name/id are fixed char arrays and never NULL). */
static void test_emit_null_arguments_fallback(void)
{
   cap_t cap = {0};
   parsed_response_t p;
   memset(&p, 0, sizeof(p));
   p.is_tool_call = 1;
   p.call_count = 1;
   snprintf(p.calls[0].id, sizeof(p.calls[0].id), "%s", "call_1");
   snprintf(p.calls[0].name, sizeof(p.calls[0].name), "%s", "f");
   p.calls[0].arguments = NULL; /* exercise the fallback */
   openai_responses_emit_policed(&p, "resp_1", "claude-test", 12345L, cap_emit, &cap);
   assert(cap.count == 5);
   /* args.delta (index 1) carries the decoded delta "{}" */
   cJSON *delta_ev = cJSON_Parse(cap.data[1]);
   assert(delta_ev != NULL);
   const cJSON *delta = cJSON_GetObjectItemCaseSensitive(delta_ev, "delta");
   assert(cJSON_IsString(delta) && strcmp(delta->valuestring, "{}") == 0);
   cJSON_Delete(delta_ev);
   /* .done (index 2) is a separate format path — verify it also carries "{}". */
   cJSON *done_ev = cJSON_Parse(cap.data[2]);
   assert(done_ev != NULL);
   const cJSON *full = cJSON_GetObjectItemCaseSensitive(done_ev, "arguments");
   assert(cJSON_IsString(full) && strcmp(full->valuestring, "{}") == 0);
   cJSON_Delete(done_ev);
   free_parsed(&p); /* arguments was NULL; free(NULL) is a no-op — kept for convention */
   PASS("emit_null_arguments_fallback");
}

/* Metadata propagation: id/model/created must appear verbatim in the
 * response.completed payload so Codex can correlate the streamed turn. */
static void test_emit_metadata_propagation(void)
{
   cap_t cap = {0};
   const char *names[] = {"web_search"};
   const char *ids[] = {"call_1"};
   const char *args[] = {"{\"q\":\"x\"}"};
   parsed_response_t p;
   seed_parsed(&p, 1, names, ids, args, "tool_calls");
   openai_responses_emit_policed(&p, "resp_meta_42", "gpt-5.5", 99999L, cap_emit, &cap);
   const char *completed = cap.data[cap.count - 1];
   assert(strcmp(cap.events[cap.count - 1], "response.completed") == 0);
   assert(strstr(completed, "resp_meta_42") != NULL);
   assert(strstr(completed, "gpt-5.5") != NULL);
   assert(strstr(completed, "99999") != NULL); /* created timestamp, verbatim */
   free_parsed(&p);
   PASS("emit_metadata_propagation");
}

/* Null parsed is tolerated (no crash): a single empty-output completed.
 * Guards against a future caller passing a police error result. */
static void test_emit_null_parsed(void)
{
   cap_t cap = {0};
   openai_responses_emit_policed(NULL, "resp_1", "claude-test", 12345L, cap_emit, &cap);
   assert(cap.count == 1);
   assert_no_created(&cap);
   assert(strcmp(cap.events[0], "response.completed") == 0);
   assert(strstr(cap.data[0], "\"output\":[]") != NULL);
   /* NULL parsed defaults usage to 0. */
   assert(strstr(cap.data[0], "\"input_tokens\":0") != NULL);
   assert(strstr(cap.data[0], "\"output_tokens\":0") != NULL);
   PASS("emit_null_parsed");
}

/* A NULL emit callback is tolerated (fail closed, no crash). */
static void test_emit_null_callback(void)
{
   const char *names[] = {"web_search"};
   const char *ids[] = {"call_1"};
   const char *args[] = {"{\"q\":\"x\"}"};
   parsed_response_t p;
   seed_parsed(&p, 1, names, ids, args, "tool_calls");
   openai_responses_emit_policed(&p, "resp_1", "claude-test", 12345L, NULL, NULL);
   free_parsed(&p);
   PASS("emit_null_callback");
}

/* Arguments are propagated verbatim (byte-for-byte fidelity for the
 * tool_use contract — Codex's executor stringifies and re-parses the
 * JSON; a re-serialization that re-ordered keys or reformatted
 * whitespace would still parse but could break string equality
 * assertions in client code). */
static void test_emit_arguments_byte_fidelity(void)
{
   cap_t cap = {0};
   const char *names[] = {"f"};
   const char *ids[] = {"c1"};
   const char *args[] = {"{\"b\":2,\"a\":1,\"nested\":{\"x\":\"y\"}}"};
   parsed_response_t p;
   seed_parsed(&p, 1, names, ids, args, "tool_calls");
   const char *original = args[0];
   openai_responses_emit_policed(&p, "resp_1", "claude-test", 12345L, cap_emit, &cap);
   /* The args.delta event (index 1) carries the arguments as the JSON-string
    * `delta` field. Byte fidelity: after parsing the event JSON, the decoded
    * `delta` string equals the original arguments byte-for-byte (no key
    * reorder, no whitespace reflow). The same holds for the `.done` event
    * at index 2. */
   assert(strcmp(cap.events[1], "response.function_call_arguments.delta") == 0);
   cJSON *delta_ev = cJSON_Parse(cap.data[1]);
   assert(delta_ev != NULL);
   const cJSON *delta = cJSON_GetObjectItemCaseSensitive(delta_ev, "delta");
   assert(cJSON_IsString(delta) && delta->valuestring != NULL);
   assert(strcmp(delta->valuestring, original) == 0);
   cJSON_Delete(delta_ev);

   assert(strcmp(cap.events[2], "response.function_call_arguments.done") == 0);
   cJSON *done_ev = cJSON_Parse(cap.data[2]);
   assert(done_ev != NULL);
   const cJSON *full = cJSON_GetObjectItemCaseSensitive(done_ev, "arguments");
   assert(cJSON_IsString(full) && full->valuestring != NULL);
   assert(strcmp(full->valuestring, original) == 0);
   cJSON_Delete(done_ev);

   free_parsed(&p);
   PASS("emit_arguments_byte_fidelity");
}

/* Escaping / UTF-8 fidelity: arguments containing embedded quotes, a
 * backslash, a newline, and a multi-byte UTF-8 sequence must decode
 * byte-for-byte from both the delta and done events (the JSON-string
 * wire-escaping must round-trip exactly). */
static void test_emit_arguments_escapes_utf8(void)
{
   cap_t cap = {0};
   const char *names[] = {"f"};
   const char *ids[] = {"c1"};
   /* quote, backslash, newline, and "café 日本" (multi-byte UTF-8). */
   const char *args[] = {"{\"s\":\"a\\\"b\\\\c\\nd café 日本\"}"};
   parsed_response_t p;
   seed_parsed(&p, 1, names, ids, args, "tool_calls");
   const char *original = args[0];
   openai_responses_emit_policed(&p, "resp_1", "claude-test", 12345L, cap_emit, &cap);

   cJSON *delta_ev = cJSON_Parse(cap.data[1]);
   assert(delta_ev != NULL);
   const cJSON *delta = cJSON_GetObjectItemCaseSensitive(delta_ev, "delta");
   assert(cJSON_IsString(delta) && strcmp(delta->valuestring, original) == 0);
   cJSON_Delete(delta_ev);

   cJSON *done_ev = cJSON_Parse(cap.data[2]);
   assert(done_ev != NULL);
   const cJSON *full = cJSON_GetObjectItemCaseSensitive(done_ev, "arguments");
   assert(cJSON_IsString(full) && strcmp(full->valuestring, original) == 0);
   cJSON_Delete(done_ev);

   free_parsed(&p);
   PASS("emit_arguments_escapes_utf8");
}

int main(void)
{
   printf("test_openai_chat_policed:\n");
   test_emit_omits_created_envelope();
   test_emit_surviving_tool_call();
   test_emit_namespaced_tool_call();
   test_emit_plain_tool_call_has_no_namespace();
   test_emit_multiple_surviving_tool_calls();
   test_emit_all_dropped_empty_output();
   test_emit_null_arguments_fallback();
   test_emit_metadata_propagation();
   test_emit_null_parsed();
   test_emit_null_callback();
   test_emit_arguments_byte_fidelity();
   test_emit_arguments_escapes_utf8();
   printf("all openai_chat_policed tests passed\n");
   return 0;
}
