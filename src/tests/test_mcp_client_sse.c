#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "aimee/protocols/mcp/mcp_client.h"
#include "support/mock_agent_http.h"

typedef struct
{
   pthread_mutex_t lock;
   pthread_cond_t cond;
   agent_http_stream_cb callback;
   void *callback_userdata;
   int running;
   int require_auth;
   int close_first_stream;
   int stream_connects;
   int auth_ok_get;
   int auth_ok_post;
} mock_sse_server_t;

static mock_sse_server_t *g_server = NULL;

static int request_id(const char *req)
{
   const char *id = strstr(req, "\"id\":");
   return id ? atoi(id + 5) : 0;
}

static int send_event_locked(mock_sse_server_t *server, const char *event, const char *data)
{
   char frame[4096];
   int n = snprintf(frame, sizeof(frame), "event: %s\ndata: %s\n\n", event, data);
   if (n <= 0 || (size_t)n >= sizeof(frame) || !server->callback)
      return -1;
   return server->callback(frame, (size_t)n, server->callback_userdata);
}

static int emit_message_when_connected(mock_sse_server_t *server, const char *json)
{
   pthread_mutex_lock(&server->lock);
   while (server->running && !server->callback)
      pthread_cond_wait(&server->cond, &server->lock);
   int rc = server->running ? send_event_locked(server, "message", json) : -1;
   pthread_mutex_unlock(&server->lock);
   return rc;
}

static int mock_stream_handler(const char *url, const char *extra_headers,
                               agent_http_stream_cb callback, void *userdata, int timeout_ms)
{
   (void)timeout_ms;
   mock_sse_server_t *server = g_server;
   assert(server != NULL);
   assert(strcmp(url, "http://127.0.0.1/mock/sse") == 0);

   int has_auth =
       extra_headers && strstr(extra_headers, "Authorization: Bearer secret-token") != NULL;

   pthread_mutex_lock(&server->lock);
   server->stream_connects++;
   if (server->require_auth && !has_auth)
      server->auth_ok_get = 0;
   server->callback = callback;
   server->callback_userdata = userdata;
   pthread_cond_broadcast(&server->cond);
   int close_now = server->close_first_stream && server->stream_connects == 1;
   int rc = send_event_locked(server, "endpoint", "http://127.0.0.1/mock/message?sessionId=test");
   if (close_now)
   {
      server->callback = NULL;
      server->callback_userdata = NULL;
      pthread_cond_broadcast(&server->cond);
      pthread_mutex_unlock(&server->lock);
      return rc == 0 ? -1 : rc;
   }
   while (server->running)
      pthread_cond_wait(&server->cond, &server->lock);
   server->callback = NULL;
   server->callback_userdata = NULL;
   pthread_mutex_unlock(&server->lock);
   return 200;
}

static int mock_post_handler(const char *url, const char *auth_header, const char *body,
                             char **response_buf, int timeout_ms, const char *extra_headers)
{
   (void)timeout_ms;
   (void)extra_headers;
   mock_sse_server_t *server = g_server;
   assert(server != NULL);
   assert(strcmp(url, "http://127.0.0.1/mock/message?sessionId=test") == 0);

   if (server->require_auth &&
       (!auth_header || strcmp(auth_header, "Authorization: Bearer secret-token") != 0))
      server->auth_ok_post = 0;

   int id = request_id(body);
   const char *json = NULL;
   if (strstr(body, "\"method\":\"initialize\""))
      json = "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"capabilities\":{}}}";
   else if (strstr(body, "\"method\":\"tools/list\""))
      json = "{\"jsonrpc\":\"2.0\",\"id\":2,\"result\":{\"tools\":[{\"name\":\"echo\"}]}}";
   else if (strstr(body, "\"method\":\"tools/call\""))
      json = "{\"jsonrpc\":\"2.0\",\"id\":3,\"result\":{\"content\":[{\"type\":\"text\",\"text\":"
             "\"hello from sse\"}]}}";

   assert(json != NULL);
   assert(id > 0);
   assert(emit_message_when_connected(server, json) == 0);
   if (response_buf)
      *response_buf = strdup("{\"ok\":true}");
   return 202;
}

static void mock_server_start(mock_sse_server_t *server, int require_auth, int close_first_stream)
{
   memset(server, 0, sizeof(*server));
   pthread_mutex_init(&server->lock, NULL);
   pthread_cond_init(&server->cond, NULL);
   server->running = 1;
   server->require_auth = require_auth;
   server->close_first_stream = close_first_stream;
   server->auth_ok_get = 1;
   server->auth_ok_post = 1;
   g_server = server;
   mock_agent_http_reset();
   mock_agent_http_set_stream_handler(mock_stream_handler);
   mock_agent_http_set_post_handler(mock_post_handler);
}

static void mock_server_stop(mock_sse_server_t *server)
{
   pthread_mutex_lock(&server->lock);
   server->running = 0;
   pthread_cond_broadcast(&server->cond);
   pthread_mutex_unlock(&server->lock);
   mock_agent_http_reset();
   g_server = NULL;
   pthread_cond_destroy(&server->cond);
   pthread_mutex_destroy(&server->lock);
}

static void test_sse_happy_path_with_auth(void)
{
   mock_sse_server_t server;
   mock_server_start(&server, 1, 0);

   mcp_transport_t *transport = mcp_transport_sse_open("http://127.0.0.1/mock/sse", "secret-token");
   assert(transport != NULL);

   mcp_client_session_t session;
   assert(mcp_client_session_init(&session, "sse", transport) == 0);
   assert(mcp_client_initialize(&session, 3000) == 0);
   assert(mcp_client_list_tools(&session, 3000) == 0);

   cJSON *args = cJSON_CreateObject();
   assert(args != NULL);
   cJSON_AddStringToObject(args, "text", "hello");
   cJSON *result = NULL;
   assert(mcp_client_call_tool(&session, "echo", args, 3000, &result, NULL, 0) == 0);
   assert(result != NULL);

   cJSON *content = cJSON_GetObjectItem(result, "content");
   assert(cJSON_IsArray(content));
   cJSON *first = cJSON_GetArrayItem(content, 0);
   assert(cJSON_IsObject(first));
   cJSON *text = cJSON_GetObjectItem(first, "text");
   assert(cJSON_IsString(text));
   assert(strcmp(text->valuestring, "hello from sse") == 0);
   assert(server.auth_ok_get == 1);
   assert(server.auth_ok_post == 1);

   cJSON_Delete(result);
   cJSON_Delete(args);
   mock_server_stop(&server);
   mcp_transport_close(transport);
}

static void test_sse_reconnects_after_stream_drop(void)
{
   mock_sse_server_t server;
   mock_server_start(&server, 0, 1);

   mcp_transport_t *transport = mcp_transport_sse_open("http://127.0.0.1/mock/sse", NULL);
   assert(transport != NULL);

   mcp_client_session_t session;
   assert(mcp_client_session_init(&session, "sse", transport) == 0);
   assert(mcp_client_initialize(&session, 3000) == 0);
   assert(server.stream_connects >= 2);

   mock_server_stop(&server);
   mcp_transport_close(transport);
}

int main(void)
{
   printf("test_mcp_client_sse\n");
   test_sse_happy_path_with_auth();
   test_sse_reconnects_after_stream_drop();
   printf("  all tests passed\n");
   return 0;
}
