#include "server_mcp_delegate.h"
#include "aimee.h"
#include "modules/git/mcp_git.h"
#include "request_context.h"
#include <aimee/ir/session_event.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

static char cwd[MAX_PATH_LEN], override[128], caller_cwd[MAX_PATH_LEN], caller_session[128];
static int worktree, caller_worktree, resolver_result, resolver_error;
static int resolved, launched, errors, check_context = 1;
static const char *resolver_session;
static int owned_workspace, identity_fd;
static request_context_t native_owner;
static int native_dispatches;
static int native_status = 200;
const request_context_t *request_context_get(void)
{
   return native_owner.model_execution ? &native_owner : NULL;
}
int loopback_rpc(const char *wire, int length, char *response, int capacity, uint32_t capabilities)
{
   assert(length == (int)strlen(wire) && capabilities == CAP_DELEGATE);
   cJSON *request = cJSON_Parse(wire);
   assert(request);
   assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(request, "method")), "mcp.call"));
   assert(
       !strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(request, "session_id")), "host-session"));
   const char *tool = cJSON_GetStringValue(cJSON_GetObjectItem(request, "tool"));
   assert(!strcmp(tool, "delegate") || !strcmp(tool, "delegate_status"));
   cJSON *arguments = cJSON_GetObjectItem(request, "arguments");
   assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(arguments, "session_id")),
                  "forged-session"));
   assert(!cJSON_GetObjectItem(request, "capabilities"));
   native_dispatches++;
   snprintf(response, capacity, "{\"status\":\"ok\",\"job_id\":17}");
   cJSON_Delete(request);
   return native_status;
}
int workspace_turn_resource_identity(const char *path, int *fd, char digest[65])
{
   assert(!strcmp(path, cwd));
   *fd = -1;
   if (!owned_workspace)
      return -1;
   *fd = identity_fd = open("/dev/null", O_RDONLY);
   assert(*fd >= 0);
   memset(digest, 'a', 64);
   digest[64] = 0;
   return 0;
}
static cJSON *forwarded;
static char verdict[32], reason[32], error_text[256];
uint32_t server_capability_for_method(const char *method)
{
   assert(!strcmp(method, "delegate"));
   return CAP_CHAT;
}
void server_mcp_served_outcome(const char *status, const char *why)
{
   snprintf(verdict, sizeof verdict, "%s", status);
   snprintf(reason, sizeof reason, "%s", why);
}
int server_send_error(server_conn_t *conn, const char *message, const char *request_id)
{
   (void)conn;
   (void)request_id;
   errors++;
   snprintf(error_text, sizeof error_text, "%s", message);
   return -7;
}
const char *session_id(void)
{
   return override[0] ? override : "default-session";
}
int session_id_override_active(void)
{
   return override[0] != 0;
}
void session_id_set_override(const char *session)
{
   snprintf(override, sizeof override, "%s", session ? session : "");
}
void session_id_clear_override(void)
{
   override[0] = 0;
}
const char *run_cmd_get_cwd(void)
{
   return cwd[0] ? cwd : NULL;
}
void run_cmd_set_cwd(const char *path)
{
   snprintf(cwd, sizeof cwd, "%s", path ? path : "");
}
int mcp_git_get_worktree(void)
{
   return worktree;
}
void mcp_git_set_worktree(int value)
{
   worktree = value;
}
int mcp_chdir_git_root(char *old_cwd, size_t old_length, cJSON *args, char **mismatch)
{
   (void)args;
   assert(!old_cwd && !old_length);
   assert(!strcmp(session_id(), resolver_session));
   resolved++;
   run_cmd_set_cwd("/resolved/worktree");
   mcp_git_set_worktree(!caller_worktree);
   session_id_set_override("resolver-internal-session");
   if (resolver_error)
      *mismatch = strdup("expected worktree is inaccessible");
   return resolver_result;
}
static void assert_caller_context(void)
{
   assert(!strcmp(cwd, caller_cwd));
   assert(!strcmp(override, caller_session));
   assert(mcp_git_get_worktree() == caller_worktree);
}
int handle_delegate(server_ctx_t *ctx, server_conn_t *conn, cJSON *request)
{
   (void)ctx;
   (void)conn;
   if (check_context)
      assert_caller_context();
   aimee_request_t canonical = {0};
   assert(!cJSON_HasObjectItem(request, "prompt"));
   assert(!aimee_ir_request_parse(cJSON_GetObjectItem(request, "request"), &canonical));
   assert(canonical.n_messages == 1 && !strcmp(canonical.messages[0].role, "user"));
   assert(!strcmp(canonical.messages[0].blocks[0].text, "Inspect the branch"));
   aimee_request_free(&canonical);
   launched++;
   forwarded = cJSON_Duplicate(request, 1);
   assert(forwarded);
   return 17;
}
static void reset(void)
{
   cJSON_Delete(forwarded);
   forwarded = NULL;
   resolved = launched = errors = resolver_error = 0;
   owned_workspace = 0;
   identity_fd = -1;
   resolver_result = 1;
   check_context = 1;
   verdict[0] = reason[0] = error_text[0] = 0;
   snprintf(caller_cwd, sizeof caller_cwd, "%s", "/caller/worktree");
   snprintf(caller_session, sizeof caller_session, "%s", "caller-session");
   snprintf(cwd, sizeof cwd, "%s", caller_cwd);
   snprintf(override, sizeof override, "%s", caller_session);
   worktree = caller_worktree = 1;
   resolver_session = "transport-session";
}
static cJSON *arguments(void)
{
   cJSON *args = cJSON_Parse("{\"role\":\"review\",\"prompt\":\"Inspect the branch\","
                             "\"persona\":\"reviewer\"}");
   assert(args);
   return args;
}
static void assert_string(const char *key, const char *expected)
{
   const char *value = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(forwarded, key));
   assert(value && !strcmp(value, expected));
}
static void test_routing(void)
{
   for (int enabled = 0; enabled < 2; enabled++)
   {
      reset();
      cJSON *args = arguments();
      cJSON_AddStringToObject(args, "cwd", "/explicit/worktree");
      cJSON_AddStringToObject(args, "via", "configured-agent");
      cJSON_AddStringToObject(args, "scope", "whole_task");
      cJSON_AddBoolToObject(args, "handoff_json", enabled);
      cJSON_AddBoolToObject(args, "tools", enabled);
      cJSON_AddStringToObject(args, "session_id", "model-forged-session");
      server_conn_t conn = {.capabilities = CAP_CHAT};
      int result = handle_mcp_delegate_call(NULL, &conn, args, "transport-session");
      assert(result == 17 && launched == 1 && !resolved && !errors);
      assert_string("method", "delegate");
      assert_string("via", "configured-agent");
      assert_string("scope", "whole_task");
      assert_string("cwd", "/explicit/worktree");
      assert_string("session_id", "transport-session");
      assert(enabled ? cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(forwarded, "handoff_json"))
                     : cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(forwarded, "handoff_json")));
      assert(enabled ? cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(forwarded, "tools"))
                     : cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(forwarded, "tools")));
      assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(forwarded, "background")));
      assert_caller_context();
      cJSON_Delete(args);
   }
}
static void test_resolution(void)
{
   reset();
   owned_workspace = 1;
   cJSON *owned_args = arguments();
   server_conn_t owned_conn = {.capabilities = CAP_CHAT};
   int owned_result = handle_mcp_delegate_call(NULL, &owned_conn, owned_args, "transport-session");
   assert(owned_result == 17 && launched == 1 && !resolved && !errors);
   assert_string("cwd", caller_cwd);
   assert_string("session_id", "transport-session");
   errno = 0;
   assert(fcntl(identity_fd, F_GETFD) == -1 && errno == EBADF);
   assert_caller_context();
   cJSON_Delete(owned_args);
   for (int active = 0; active < 3; active++)
   {
      reset();
      if (!active)
         caller_cwd[0] = caller_session[0] = cwd[0] = override[0] = 0;
      if (active == 2)
      {
         memset(caller_session, 's', sizeof caller_session - 1);
         caller_session[sizeof caller_session - 1] = 0;
         snprintf(override, sizeof override, "%s", caller_session);
      }
      cJSON *args = arguments();
      server_conn_t conn = {.capabilities = CAP_CHAT};
      resolver_session = active == 1   ? "transport-session"
                         : active == 2 ? caller_session
                                       : "default-session";
      int result =
          handle_mcp_delegate_call(NULL, &conn, args, active == 1 ? "transport-session" : NULL);
      assert(result == 17 && launched == 1 && resolved == 1 && !errors);
      assert_string("cwd", "/resolved/worktree");
      assert(!cJSON_GetObjectItemCaseSensitive(forwarded, "via"));
      assert(!cJSON_GetObjectItemCaseSensitive(forwarded, "scope"));
      assert(!cJSON_GetObjectItemCaseSensitive(forwarded, "handoff_json"));
      assert(!cJSON_GetObjectItemCaseSensitive(forwarded, "tools"));
      if (active != 1)
         assert(!cJSON_GetObjectItemCaseSensitive(forwarded, "session_id"));
      else
         assert_string("session_id", "transport-session");
      assert_caller_context();
      cJSON_Delete(args);
   }
   reset();
   resolver_result = 0; /* No registered default remains a supported server-owned choice. */
   cJSON *args = arguments();
   server_conn_t conn = {.capabilities = CAP_CHAT};
   int result = handle_mcp_delegate_call(NULL, &conn, args, "transport-session");
   assert(result == 17 && launched == 1 && resolved == 1 && !errors);
   assert(!cJSON_GetObjectItemCaseSensitive(forwarded, "cwd"));
   assert_caller_context();
   cJSON_Delete(args);
}
static void test_refusal(void)
{
   for (int mode = 0; mode < 3; mode++)
   {
      reset();
      resolver_result = mode == 0 ? -1 : mode == 1 ? -2 : 1;
      resolver_error = mode != 0;
      check_context = 0;
      cJSON *args = arguments();
      server_conn_t conn = {.capabilities = CAP_CHAT};
      int result = handle_mcp_delegate_call(NULL, &conn, args, "transport-session");
      assert(result == -7 && !launched && resolved == 1 && errors == 1);
      assert(!strcmp(verdict, "refused") && !strcmp(reason, "context"));
      assert(error_text[0]);
      assert_caller_context();
      cJSON_Delete(args);
   }
   reset();
   cJSON *args = arguments();
   server_conn_t conn = {0};
   int denied = handle_mcp_delegate_call(NULL, &conn, args, "transport-session");
   assert(denied == -7 && !launched && !resolved && errors == 1);
   assert(!strcmp(reason, "role"));
   conn.capabilities = CAP_CHAT;
   cJSON_DeleteItemFromObjectCaseSensitive(args, "persona");
   int missing = handle_mcp_delegate_call(NULL, &conn, args, "transport-session");
   assert(missing == -7 && !launched && !resolved && errors == 2);
   assert(!strcmp(reason, "bad_args"));
   assert_caller_context();
   cJSON_Delete(args);
}
static void test_native_dispatch(void)
{
   cJSON *args = arguments();
   cJSON_AddStringToObject(args, "session_id", "forged-session");
   assert(!server_mcp_delegate_native("delegate", args, "host-session"));
   native_owner.model_execution = 1;
   native_owner.capabilities = CAP_DELEGATE;
   assert(!server_mcp_delegate_native("delegate", args, "host-session"));
   native_owner.vault_transport = ATTEST_WEBCHAT_TRUSTED;
   assert(!server_mcp_delegate_native("git_push", args, "host-session"));
   assert(!server_mcp_delegate_native("delegate", args, NULL));
   assert(!native_dispatches);
   const char *tools[] = {"delegate", "delegate_status"};
   for (size_t i = 0; i < 2; i++)
   {
      char *response = server_mcp_delegate_native(tools[i], args, "host-session");
      assert(response && strstr(response, "\"job_id\":17"));
      free(response);
   }
   assert(native_dispatches == 2);
   native_status = 503;
   char *failed = server_mcp_delegate_native("delegate", args, "host-session");
   assert(failed && !strncmp(failed, "error: ", 7));
   free(failed);
   native_status = 200;
   memset(&native_owner, 0, sizeof native_owner);
   cJSON_Delete(args);
}
int main(int argc, char **argv)
{
   if (argc == 1 || !strcmp(argv[1], "native"))
      test_native_dispatch();
   if (argc == 1 || !strcmp(argv[1], "routing"))
      test_routing();
   if (argc == 1 || !strcmp(argv[1], "scope"))
      test_resolution();
   if (argc == 1 || !strcmp(argv[1], "refusal"))
      test_refusal();
   cJSON_Delete(forwarded);
   puts("MCP delegate: model route/options, scoped cwd/session and worktree refusals passed");
   return 0;
}
