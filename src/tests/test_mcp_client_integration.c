#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "cJSON.h"
#include "aimee/protocols/mcp/mcp_client.h"

#ifdef AIMEE_POSIX
typedef struct
{
   pid_t pid;
   int in_fd;
   int out_fd;
   char *read_buf;
   size_t read_len;
   size_t read_cap;
} stdio_state_view_t;
#endif

static int path_exists(const char *path)
{
   FILE *f = fopen(path, "rb");
   if (!f)
      return 0;
   fclose(f);
   return 1;
}

static const char *mock_server_path(void)
{
#ifdef MCP_MOCK_SERVER_PATH
   if (path_exists(MCP_MOCK_SERVER_PATH))
      return MCP_MOCK_SERVER_PATH;
   static char root_path[256];
   if (MCP_MOCK_SERVER_PATH[0] != '/')
   {
      snprintf(root_path, sizeof(root_path), "src/%s", MCP_MOCK_SERVER_PATH);
      if (path_exists(root_path))
         return root_path;
   }
   return MCP_MOCK_SERVER_PATH;
#else
   if (path_exists("./build/obj/tests/mock-mcp-server"))
      return "./build/obj/tests/mock-mcp-server";
   return "./src/build/obj/tests/mock-mcp-server";
#endif
}

static void test_stdio_happy_path(void)
{
   const char *argv[] = {mock_server_path(), "happy", NULL};
   mcp_transport_t *transport = mcp_transport_stdio_open(argv, NULL);
   assert(transport != NULL);

   mcp_client_session_t session;
   assert(mcp_client_session_init(&session, "mock", transport) == 0);
   assert(mcp_client_initialize(&session, 1000) == 0);
   assert(mcp_client_list_tools(&session, 1000) == 0);

   cJSON *result = NULL;
   char err[128] = "";
   assert(mcp_client_call_tool(&session, "echo", NULL, 1000, &result, err, sizeof(err)) == 0);
   assert(result != NULL);
   cJSON *content = cJSON_GetObjectItemCaseSensitive(result, "content");
   cJSON *first = cJSON_GetArrayItem(content, 0);
   cJSON *text = cJSON_GetObjectItemCaseSensitive(first, "text");
   assert(cJSON_IsString(text));
   assert(strcmp(text->valuestring, "hello from mock") == 0);
   cJSON_Delete(result);

   mcp_client_session_close(&session);
}

static void test_stdio_eof_during_recv(void)
{
   const char *argv[] = {mock_server_path(), "eof_after_init", NULL};
   mcp_transport_t *transport = mcp_transport_stdio_open(argv, NULL);
   assert(transport != NULL);

   mcp_client_session_t session;
   assert(mcp_client_session_init(&session, "mock", transport) == 0);
   assert(mcp_client_initialize(&session, 1000) == 0);
   assert(mcp_client_list_tools(&session, 1000) == -1);

   mcp_client_session_close(&session);
}

static void test_stdio_bad_exec_path(void)
{
   const char *argv[] = {"/definitely/not/a/real/mcp-server", NULL};
   mcp_transport_t *transport = mcp_transport_stdio_open(argv, NULL);
   assert(transport != NULL);

   mcp_client_session_t session;
   assert(mcp_client_session_init(&session, "missing", transport) == 0);
   assert(mcp_client_initialize(&session, 1000) == -1);
   mcp_client_session_close(&session);
}

static void test_stdio_malformed_json(void)
{
   const char *argv[] = {mock_server_path(), "malformed", NULL};
   mcp_transport_t *transport = mcp_transport_stdio_open(argv, NULL);
   assert(transport != NULL);

   mcp_client_session_t session;
   assert(mcp_client_session_init(&session, "mock", transport) == 0);
   assert(mcp_client_initialize(&session, 1000) == -1);
   mcp_client_session_close(&session);
}

static void test_stdio_close_reaps_child(void)
{
#ifdef AIMEE_POSIX
   const char *argv[] = {mock_server_path(), "flood", NULL};
   mcp_transport_t *transport = mcp_transport_stdio_open(argv, NULL);
   assert(transport != NULL);

   mcp_client_session_t session;
   assert(mcp_client_session_init(&session, "mock", transport) == 0);
   assert(mcp_client_initialize(&session, 1000) == 0);

   stdio_state_view_t *view = (stdio_state_view_t *)transport->state;
   pid_t pid = view->pid;
   assert(pid > 0);

   mcp_client_session_close(&session);

   errno = 0;
   assert(waitpid(pid, NULL, WNOHANG) == -1);
   assert(errno == ECHILD);
#endif
}

int main(void)
{
   printf("test_mcp_client_integration\n");
   test_stdio_happy_path();
   test_stdio_eof_during_recv();
   test_stdio_bad_exec_path();
   test_stdio_malformed_json();
   test_stdio_close_reaps_child();
   printf("  all tests passed\n");
   return 0;
}
