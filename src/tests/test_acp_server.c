/* test_acp_server.c: unit tests for the inbound ACP server dispatch
 * (src/acp_server.c) — pure, no stdio. */
#include "acp_server.h"
#include "cJSON.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static cJSON *parse(const char *s)
{
   cJSON *o = cJSON_Parse(s);
   assert(o != NULL);
   return o;
}

static void test_initialize(void)
{
   char out[8192];
   int rc = acp_server_handle_line(
       "{\"jsonrpc\":\"2.0\",\"method\":\"initialize\",\"params\":{},\"id\":1}", out, sizeof(out));
   assert(rc == 1);
   cJSON *o = parse(out);
   assert(strcmp(cJSON_GetObjectItem(o, "jsonrpc")->valuestring, "2.0") == 0);
   assert(cJSON_GetObjectItem(o, "id")->valueint == 1);
   cJSON *result = cJSON_GetObjectItem(o, "result");
   assert(result && cJSON_GetObjectItem(result, "protocolVersion")->valueint == 1);
   cJSON *info = cJSON_GetObjectItem(result, "agentInfo");
   assert(strcmp(cJSON_GetObjectItem(info, "name")->valuestring, "aimee") == 0);
   cJSON *acaps = cJSON_GetObjectItem(result, "agentCapabilities");
   assert(acaps && cJSON_IsTrue(cJSON_GetObjectItem(acaps, "loadSession")));
   assert(cJSON_GetObjectItem(acaps, "promptCapabilities") != NULL);
   assert(cJSON_IsArray(cJSON_GetObjectItem(result, "authMethods")));
   cJSON *cmds = cJSON_GetObjectItem(result, "slashCommands");
   assert(cJSON_IsArray(cmds) && cJSON_GetArraySize(cmds) >= 5);
   /* The request id is preserved verbatim. */
   assert(cJSON_GetObjectItem(o, "error") == NULL);
   cJSON_Delete(o);
   printf("  PASS: initialize\n");
}

static void test_string_id_preserved(void)
{
   char out[8192];
   int rc = acp_server_handle_line("{\"jsonrpc\":\"2.0\",\"method\":\"initialize\",\"id\":\"abc\"}",
                                   out, sizeof(out));
   assert(rc == 1);
   cJSON *o = parse(out);
   assert(strcmp(cJSON_GetObjectItem(o, "id")->valuestring, "abc") == 0);
   cJSON_Delete(o);
   printf("  PASS: string_id_preserved\n");
}

static void test_session_new(void)
{
   char out[8192];
   int rc = acp_server_handle_line(
       "{\"jsonrpc\":\"2.0\",\"method\":\"session/new\",\"params\":{},\"id\":2}", out, sizeof(out));
   assert(rc == 1);
   cJSON *o = parse(out);
   assert(cJSON_GetObjectItem(o, "id")->valueint == 2);
   cJSON *result = cJSON_GetObjectItem(o, "result");
   assert(result != NULL);
   cJSON *sid = cJSON_GetObjectItem(result, "sessionId");
   assert(cJSON_IsString(sid) && sid->valuestring[0]);
   assert(cJSON_GetObjectItem(o, "error") == NULL);
   cJSON_Delete(o);
   printf("  PASS: session_new\n");
}

static void test_unknown_method_errors(void)
{
   char out[8192];
   int rc = acp_server_handle_line("{\"jsonrpc\":\"2.0\",\"method\":\"frobnicate\",\"id\":7}", out,
                                   sizeof(out));
   assert(rc == 1);
   cJSON *o = parse(out);
   assert(cJSON_GetObjectItem(o, "id")->valueint == 7);
   cJSON *err = cJSON_GetObjectItem(o, "error");
   assert(err && cJSON_GetObjectItem(err, "code")->valueint == -32601);
   assert(cJSON_GetObjectItem(o, "result") == NULL);
   cJSON_Delete(o);
   printf("  PASS: unknown_method_errors\n");
}

static void test_notification_no_response(void)
{
   char out[8192];
   /* Unknown method with no id is a notification: no reply. */
   int rc =
       acp_server_handle_line("{\"jsonrpc\":\"2.0\",\"method\":\"cancelled\"}", out, sizeof(out));
   assert(rc == 0);
   assert(out[0] == '\0');
   printf("  PASS: notification_no_response\n");
}

static void test_invalid_request_with_id(void)
{
   char out[8192];
   /* No method but has an id -> invalid request. */
   int rc = acp_server_handle_line("{\"jsonrpc\":\"2.0\",\"id\":3}", out, sizeof(out));
   assert(rc == 1);
   cJSON *o = parse(out);
   assert(cJSON_GetObjectItem(o, "error")->child != NULL);
   assert(cJSON_GetObjectItem(cJSON_GetObjectItem(o, "error"), "code")->valueint == -32600);
   cJSON_Delete(o);
   printf("  PASS: invalid_request_with_id\n");
}

static void test_parse_error(void)
{
   char out[8192];
   int rc = acp_server_handle_line("{not json", out, sizeof(out));
   assert(rc == 1);
   cJSON *o = parse(out);
   cJSON *err = cJSON_GetObjectItem(o, "error");
   assert(err && cJSON_GetObjectItem(err, "code")->valueint == -32700);
   assert(cJSON_IsNull(cJSON_GetObjectItem(o, "id")));
   cJSON_Delete(o);
   printf("  PASS: parse_error\n");
}

static void test_blank_line(void)
{
   char out[8192];
   assert(acp_server_handle_line("   \r\n", out, sizeof(out)) == 0);
   assert(out[0] == '\0');
   assert(acp_server_handle_line("", out, sizeof(out)) == 0);
   assert(acp_server_handle_line(NULL, out, sizeof(out)) == 0);
   printf("  PASS: blank_line\n");
}

static void test_build_initialize_null_id(void)
{
   char out[8192];
   int rc = acp_server_build_initialize(NULL, out, sizeof(out));
   assert(rc == 1);
   cJSON *o = parse(out);
   assert(cJSON_IsNull(cJSON_GetObjectItem(o, "id")));
   assert(cJSON_GetObjectItem(o, "result") != NULL);
   cJSON_Delete(o);
   printf("  PASS: build_initialize_null_id\n");
}

static void test_prompt_parse(void)
{
   char content[256], session[128], id[128];
   int rc = acp_prompt_parse(
       "{\"jsonrpc\":\"2.0\",\"method\":\"prompt/submit\","
       "\"params\":{\"content\":\"hello there\",\"sessionId\":\"acp-sess-1\"},\"id\":5}",
       content, sizeof(content), session, sizeof(session), id, sizeof(id));
   assert(rc == 1);
   assert(strcmp(content, "hello there") == 0);
   assert(strcmp(session, "acp-sess-1") == 0);
   assert(strcmp(id, "5") == 0);

   /* Non-prompt methods are not claimed by the parser. */
   assert(acp_prompt_parse("{\"jsonrpc\":\"2.0\",\"method\":\"initialize\",\"id\":1}", content,
                           sizeof(content), session, sizeof(session), id, sizeof(id)) == 0);
   assert(acp_prompt_parse("not json", content, sizeof(content), session, sizeof(session), id,
                           sizeof(id)) == 0);
   printf("  PASS: prompt_parse\n");
}

static void test_build_prompt_result(void)
{
   char out[8192];
   int rc = acp_build_prompt_result("5", "the reply", out, sizeof(out));
   assert(rc == 1);
   cJSON *o = parse(out);
   assert(cJSON_GetObjectItem(o, "id")->valueint == 5);
   cJSON *result = cJSON_GetObjectItem(o, "result");
   assert(result && strcmp(cJSON_GetObjectItem(result, "content")->valuestring, "the reply") == 0);
   cJSON_Delete(o);
   printf("  PASS: build_prompt_result\n");
}

static void test_session_load(void)
{
   char out[8192];
   int rc = acp_server_handle_line("{\"jsonrpc\":\"2.0\",\"method\":\"session/load\","
                                   "\"params\":{\"sessionId\":\"acp-sess-9\"},\"id\":4}",
                                   out, sizeof(out));
   assert(rc == 1);
   cJSON *o = parse(out);
   assert(cJSON_GetObjectItem(o, "id")->valueint == 4);
   cJSON *result = cJSON_GetObjectItem(o, "result");
   assert(result &&
          strcmp(cJSON_GetObjectItem(result, "sessionId")->valuestring, "acp-sess-9") == 0);
   assert(cJSON_GetObjectItem(o, "error") == NULL);
   cJSON_Delete(o);
   printf("  PASS: session_load\n");
}

static void test_slash_reply(void)
{
   char reply[1024];
   assert(acp_slash_reply("/help", reply, sizeof(reply)) == 1);
   assert(strstr(reply, "/skill") != NULL && strstr(reply, "/compact") != NULL);
   assert(acp_slash_reply("/help me please", reply, sizeof(reply)) == 1);
   /* Not a local command -> falls through to a normal turn. */
   assert(acp_slash_reply("/compact", reply, sizeof(reply)) == 0);
   assert(acp_slash_reply("hello", reply, sizeof(reply)) == 0);
   assert(acp_slash_reply("/helpful", reply, sizeof(reply)) == 0); /* not exactly /help */
   printf("  PASS: slash_reply\n");
}

static void test_update_notification(void)
{
   char out[8192];
   int rc = acp_build_update_notification("acp-sess-2", "hello", out, sizeof(out));
   assert(rc == 1);
   cJSON *o = parse(out);
   /* A notification has a method and no id. */
   assert(strcmp(cJSON_GetObjectItem(o, "method")->valuestring, "session/update") == 0);
   assert(cJSON_GetObjectItem(o, "id") == NULL);
   cJSON *params = cJSON_GetObjectItem(o, "params");
   assert(strcmp(cJSON_GetObjectItem(params, "sessionId")->valuestring, "acp-sess-2") == 0);
   cJSON *update = cJSON_GetObjectItem(params, "update");
   /* Standard ACP: sessionUpdate discriminator + typed text content block. */
   assert(strcmp(cJSON_GetObjectItem(update, "sessionUpdate")->valuestring,
                 "agent_message_chunk") == 0);
   cJSON *block = cJSON_GetObjectItem(update, "content");
   assert(strcmp(cJSON_GetObjectItem(block, "type")->valuestring, "text") == 0);
   assert(strcmp(cJSON_GetObjectItem(block, "text")->valuestring, "hello") == 0);
   cJSON_Delete(o);
   printf("  PASS: update_notification\n");
}

static void test_tool_update(void)
{
   char out[2048];
   /* started -> tool_call / in_progress, with the tool name as the title and the
    * caller's id as the handle. */
   assert(acp_build_tool_update("s1", "read_file-1", "started", "read_file", out, sizeof(out)) ==
          1);
   cJSON *o = parse(out);
   cJSON *u = cJSON_GetObjectItem(cJSON_GetObjectItem(o, "params"), "update");
   assert(strcmp(cJSON_GetObjectItem(u, "sessionUpdate")->valuestring, "tool_call") == 0);
   assert(strcmp(cJSON_GetObjectItem(u, "toolCallId")->valuestring, "read_file-1") == 0);
   assert(strcmp(cJSON_GetObjectItem(u, "title")->valuestring, "read_file") == 0);
   assert(strcmp(cJSON_GetObjectItem(u, "status")->valuestring, "in_progress") == 0);
   cJSON_Delete(o);
   /* completed -> tool_call_update / completed, carrying the SAME id so the client
    * can find the call it updates. */
   assert(acp_build_tool_update("s1", "read_file-1", "completed", "read_file", out, sizeof(out)) ==
          1);
   o = parse(out);
   u = cJSON_GetObjectItem(cJSON_GetObjectItem(o, "params"), "update");
   assert(strcmp(cJSON_GetObjectItem(u, "sessionUpdate")->valuestring, "tool_call_update") == 0);
   assert(strcmp(cJSON_GetObjectItem(u, "toolCallId")->valuestring, "read_file-1") == 0);
   assert(strcmp(cJSON_GetObjectItem(u, "status")->valuestring, "completed") == 0);
   cJSON_Delete(o);
   printf("  PASS: tool_update\n");
}

/* Two calls to the SAME tool in one session must not share a toolCallId.
 *
 * The id was the tool name, so an agent that read three files emitted three
 * tool_calls under the id "read_file" — a client keys tool_call_update off the id
 * to find the call it updates, so all three collapsed into one entry that flipped
 * in_progress/completed as each landed. The ids are the caller's to number; this
 * pins the contract that distinct calls get distinct handles. */
static void test_tool_call_ids_are_unique_per_call(void)
{
   char a[2048], b[2048];
   assert(acp_build_tool_update("s1", "read_file-1", "started", "read_file", a, sizeof(a)) == 1);
   assert(acp_build_tool_update("s1", "read_file-2", "started", "read_file", b, sizeof(b)) == 1);
   cJSON *oa = parse(a), *ob = parse(b);
   const char *ida =
       cJSON_GetObjectItem(cJSON_GetObjectItem(cJSON_GetObjectItem(oa, "params"), "update"),
                           "toolCallId")
           ->valuestring;
   const char *idb =
       cJSON_GetObjectItem(cJSON_GetObjectItem(cJSON_GetObjectItem(ob, "params"), "update"),
                           "toolCallId")
           ->valuestring;
   assert(strcmp(ida, idb) != 0);
   cJSON_Delete(oa);
   cJSON_Delete(ob);
   printf("  PASS: tool_call_ids_are_unique_per_call\n");
}

static void test_session_prompt_parse(void)
{
   char content[256], session[128], id[128];
   /* Standard ACP session/prompt: prompt is an array of content blocks. */
   int rc = acp_prompt_parse(
       "{\"jsonrpc\":\"2.0\",\"method\":\"session/prompt\",\"params\":{\"sessionId\":\"acp-9\","
       "\"prompt\":[{\"type\":\"text\",\"text\":\"hello \"},{\"type\":\"text\",\"text\":\"world\"}]"
       "},\"id\":7}",
       content, sizeof(content), session, sizeof(session), id, sizeof(id));
   assert(rc == 2); /* 2 == standard session/prompt */
   assert(strcmp(content, "hello world") == 0);
   assert(strcmp(session, "acp-9") == 0);
   assert(strcmp(id, "7") == 0);
   printf("  PASS: session_prompt_parse\n");
}

static void test_build_prompt_stop(void)
{
   char out[1024];
   assert(acp_build_prompt_stop("7", "end_turn", out, sizeof(out)) == 1);
   cJSON *o = parse(out);
   assert(cJSON_GetObjectItem(o, "id")->valueint == 7);
   cJSON *result = cJSON_GetObjectItem(o, "result");
   assert(result &&
          strcmp(cJSON_GetObjectItem(result, "stopReason")->valuestring, "end_turn") == 0);
   assert(cJSON_GetObjectItem(result, "content") == NULL); /* text came via the stream */
   cJSON_Delete(o);
   printf("  PASS: build_prompt_stop\n");
}

int main(void)
{
   printf("acp_server: ");
   test_prompt_parse();
   test_session_prompt_parse();
   test_tool_update();
   test_tool_call_ids_are_unique_per_call();
   test_build_prompt_result();
   test_build_prompt_stop();
   test_session_load();
   test_slash_reply();
   test_update_notification();
   test_initialize();
   test_string_id_preserved();
   test_session_new();
   test_unknown_method_errors();
   test_notification_no_response();
   test_invalid_request_with_id();
   test_parse_error();
   test_blank_line();
   test_build_initialize_null_id();
   printf("acp_server: all tests passed\n");
   return 0;
}
