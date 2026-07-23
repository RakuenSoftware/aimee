/* test_mcp_client.c: unit tests for JSON-RPC framing and the session layer.
 *
 * Uses an in-memory stub transport so we can exercise initialize/list_tools/
 * call_tool end-to-end without spawning a real subprocess. The stdio transport
 * itself is exercised by integration tests in a follow-up proposal. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "aimee/protocols/mcp/mcp_client.h"

#define PASS(name) printf("  PASS: %s\n", name)

/* --- Stub transport --------------------------------------------------------- */

typedef struct
{
   /* Scripted responses to hand back, one per recv() call. */
   char **responses;
   size_t resp_count;
   size_t resp_idx;
   /* Last request sent, for assertions. */
   char *last_sent;
} stub_state_t;

static int stub_send(mcp_transport_t *t, const char *json, size_t len)
{
   stub_state_t *s = (stub_state_t *)t->state;
   free(s->last_sent);
   s->last_sent = malloc(len + 1);
   memcpy(s->last_sent, json, len);
   s->last_sent[len] = '\0';
   return 0;
}

static int stub_recv(mcp_transport_t *t, char *buf, size_t buflen, int timeout_ms)
{
   (void)timeout_ms;
   stub_state_t *s = (stub_state_t *)t->state;
   if (s->resp_idx >= s->resp_count)
      return -1;
   const char *resp = s->responses[s->resp_idx++];
   size_t n = strlen(resp);
   if (n >= buflen)
      n = buflen - 1;
   memcpy(buf, resp, n);
   buf[n] = '\0';
   return (int)n;
}

static void stub_close(mcp_transport_t *t)
{
   stub_state_t *s = (stub_state_t *)t->state;
   if (!s)
      return;
   for (size_t i = 0; i < s->resp_count; i++)
      free(s->responses[i]);
   free(s->responses);
   free(s->last_sent);
   free(s);
   t->state = NULL;
}

static const mcp_transport_vtable_t stub_vt = {
    .send = stub_send,
    .recv = stub_recv,
    .close = stub_close,
};

/* Build a stub with the given scripted responses. |responses| is copied. */
static mcp_transport_t *stub_open(const char **responses, size_t n)
{
   mcp_transport_t *t = calloc(1, sizeof(*t));
   stub_state_t *s = calloc(1, sizeof(*s));
   s->responses = calloc(n, sizeof(char *));
   s->resp_count = n;
   for (size_t i = 0; i < n; i++)
      s->responses[i] = strdup(responses[i]);
   t->kind = MCP_TRANSPORT_STDIO;
   t->vt = &stub_vt;
   t->state = s;
   return t;
}

/* --- Framing tests ---------------------------------------------------------- */

static void test_build_request_no_params(void)
{
   char *f = mcp_jsonrpc_build_request(7, "tools/list", NULL);
   assert(f != NULL);

   /* Must be newline-terminated */
   size_t len = strlen(f);
   assert(len > 0 && f[len - 1] == '\n');

   cJSON *r = cJSON_Parse(f);
   assert(r);
   assert(strcmp(cJSON_GetObjectItem(r, "jsonrpc")->valuestring, "2.0") == 0);
   assert((int)cJSON_GetObjectItem(r, "id")->valuedouble == 7);
   assert(strcmp(cJSON_GetObjectItem(r, "method")->valuestring, "tools/list") == 0);
   assert(cJSON_GetObjectItem(r, "params") == NULL);
   cJSON_Delete(r);
   free(f);
   PASS("build_request_no_params");
}

static void test_build_request_with_params(void)
{
   cJSON *params = cJSON_CreateObject();
   cJSON_AddStringToObject(params, "name", "echo");

   char *f = mcp_jsonrpc_build_request(42, "tools/call", params);
   assert(f != NULL);
   cJSON *r = cJSON_Parse(f);
   assert(r);
   cJSON *p = cJSON_GetObjectItem(r, "params");
   assert(cJSON_IsObject(p));
   assert(strcmp(cJSON_GetObjectItem(p, "name")->valuestring, "echo") == 0);

   /* Caller retains ownership of params. */
   cJSON_Delete(params);
   cJSON_Delete(r);
   free(f);
   PASS("build_request_with_params");
}

static void test_parse_response_success(void)
{
   const char *frame = "{\"jsonrpc\":\"2.0\",\"id\":3,\"result\":{\"tools\":[]}}";
   int id = 0;
   cJSON *root = NULL;
   cJSON *result = NULL;
   int rc = mcp_jsonrpc_parse_response(frame, &id, &root, &result, NULL, 0);
   assert(rc == 0);
   assert(id == 3);
   assert(root != NULL);
   assert(result != NULL);
   assert(cJSON_IsArray(cJSON_GetObjectItem(result, "tools")));
   cJSON_Delete(root);
   PASS("parse_response_success");
}

static void test_parse_response_error(void)
{
   const char *frame =
       "{\"jsonrpc\":\"2.0\",\"id\":5,\"error\":{\"code\":-32601,\"message\":\"no such method\"}}";
   int id = 0;
   cJSON *root = NULL;
   cJSON *result = NULL;
   char err[128] = {0};
   int rc = mcp_jsonrpc_parse_response(frame, &id, &root, &result, err, sizeof(err));
   assert(rc == -1);
   assert(strstr(err, "no such method") != NULL);
   assert(root == NULL); /* freed internally on error */
   PASS("parse_response_error");
}

static void test_parse_response_invalid_json(void)
{
   char err[64] = {0};
   cJSON *root = NULL;
   cJSON *result = NULL;
   int rc = mcp_jsonrpc_parse_response("not json", NULL, &root, &result, err, sizeof(err));
   assert(rc == -1);
   assert(err[0] != 0);
   PASS("parse_response_invalid_json");
}

/* --- Session tests ---------------------------------------------------------- */

static void test_session_initialize(void)
{
   const char *responses[] = {
       "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"protocolVersion\":\"2024-11-05\"}}",
   };
   mcp_transport_t *t = stub_open(responses, 1);
   mcp_client_session_t s;
   assert(mcp_client_session_init(&s, "test", t) == 0);
   assert(mcp_client_initialize(&s, 1000) == 0);
   assert(s.initialized == 1);

   /* Request that was sent should reference method=initialize with id=1. */
   stub_state_t *st = (stub_state_t *)t->state;
   assert(st->last_sent != NULL);
   cJSON *sent = cJSON_Parse(st->last_sent);
   assert(strcmp(cJSON_GetObjectItem(sent, "method")->valuestring, "initialize") == 0);
   assert((int)cJSON_GetObjectItem(sent, "id")->valuedouble == 1);
   cJSON_Delete(sent);

   /* Calling initialize again is idempotent and does not consume a response. */
   assert(mcp_client_initialize(&s, 1000) == 0);

   mcp_client_session_close(&s);
   PASS("session_initialize");
}

static void test_session_list_tools(void)
{
   const char *responses[] = {
       "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"tools\":[{\"name\":\"echo\"}]}}",
   };
   mcp_transport_t *t = stub_open(responses, 1);
   mcp_client_session_t s;
   assert(mcp_client_session_init(&s, "test", t) == 0);
   assert(mcp_client_list_tools(&s, 1000) == 0);

   assert(s.tool_schemas != NULL);
   cJSON *tools = cJSON_GetObjectItem(s.tool_schemas, "tools");
   assert(cJSON_IsArray(tools));
   cJSON *first = cJSON_GetArrayItem(tools, 0);
   assert(strcmp(cJSON_GetObjectItem(first, "name")->valuestring, "echo") == 0);

   mcp_client_session_close(&s);
   PASS("session_list_tools");
}

static void test_session_call_tool(void)
{
   const char *responses[] = {
       "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"content\":[{\"type\":\"text\","
       "\"text\":\"hi\"}]}}",
   };
   mcp_transport_t *t = stub_open(responses, 1);
   mcp_client_session_t s;
   assert(mcp_client_session_init(&s, "test", t) == 0);

   cJSON *args = cJSON_CreateObject();
   cJSON_AddStringToObject(args, "greeting", "hi");

   cJSON *result = NULL;
   char err[128] = {0};
   int rc = mcp_client_call_tool(&s, "echo", args, 1000, &result, err, sizeof(err));
   assert(rc == 0);
   assert(result != NULL);
   cJSON *content = cJSON_GetObjectItem(result, "content");
   assert(cJSON_IsArray(content));
   cJSON_Delete(result);
   cJSON_Delete(args);

   /* Verify what we sent. */
   stub_state_t *st = (stub_state_t *)t->state;
   cJSON *sent = cJSON_Parse(st->last_sent);
   assert(strcmp(cJSON_GetObjectItem(sent, "method")->valuestring, "tools/call") == 0);
   cJSON *params = cJSON_GetObjectItem(sent, "params");
   assert(strcmp(cJSON_GetObjectItem(params, "name")->valuestring, "echo") == 0);
   cJSON *sent_args = cJSON_GetObjectItem(params, "arguments");
   assert(strcmp(cJSON_GetObjectItem(sent_args, "greeting")->valuestring, "hi") == 0);
   cJSON_Delete(sent);

   mcp_client_session_close(&s);
   PASS("session_call_tool");
}

static void test_session_call_tool_error(void)
{
   const char *responses[] = {
       "{\"jsonrpc\":\"2.0\",\"id\":1,\"error\":{\"code\":-32000,\"message\":\"boom\"}}",
   };
   mcp_transport_t *t = stub_open(responses, 1);
   mcp_client_session_t s;
   assert(mcp_client_session_init(&s, "test", t) == 0);

   cJSON *result = NULL;
   char err[128] = {0};
   int rc = mcp_client_call_tool(&s, "bad", NULL, 1000, &result, err, sizeof(err));
   assert(rc == -1);
   assert(result == NULL);
   assert(strstr(err, "boom") != NULL);

   mcp_client_session_close(&s);
   PASS("session_call_tool_error");
}

static void test_session_id_mismatch_skipped(void)
{
   /* First response has a wrong id (server notification / late reply);
    * session must skip it and match the second. */
   const char *responses[] = {
       "{\"jsonrpc\":\"2.0\",\"id\":999,\"result\":{\"ignored\":true}}",
       "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"protocolVersion\":\"2024-11-05\"}}",
   };
   mcp_transport_t *t = stub_open(responses, 2);
   mcp_client_session_t s;
   assert(mcp_client_session_init(&s, "test", t) == 0);
   assert(mcp_client_initialize(&s, 1000) == 0);
   mcp_client_session_close(&s);
   PASS("session_id_mismatch_skipped");
}

int main(void)
{
   printf("test_mcp_client\n");
   test_build_request_no_params();
   test_build_request_with_params();
   test_parse_response_success();
   test_parse_response_error();
   test_parse_response_invalid_json();
   test_session_initialize();
   test_session_list_tools();
   test_session_call_tool();
   test_session_call_tool_error();
   test_session_id_mismatch_skipped();
   printf("  all tests passed\n");
   return 0;
}
