/* test_cli_v1_delegate.c: thin-client delegate RPC marshaling tests */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "aimee.h"
#include "cli_client.h"
#include "platform_path.h"
#include "cJSON.h"

#define V1_PROTOCOL_VERSION 1

/* Include the route implementation directly so static marshal helpers are testable. */
#include "../cli_v1_routes.inc"

static void test_delegate_max_turns_marshaled(void)
{
   char *argv[] = {"review",
                   "--tools",
                   "--max-turns",
                   "40",
                   "--output",
                   "/tmp/out",
                   "inspect this bounded diff"};
   cJSON *req = marshal_delegate(7, argv);
   assert(req != NULL);
   assert(strcmp(cJSON_GetObjectItem(req, "method")->valuestring, "delegate") == 0);
   assert(strcmp(cJSON_GetObjectItem(req, "role")->valuestring, "review") == 0);
   assert(strcmp(cJSON_GetObjectItem(req, "prompt")->valuestring, "inspect this bounded diff") ==
          0);
   assert(cJSON_IsTrue(cJSON_GetObjectItem(req, "tools")));
   assert(cJSON_GetObjectItem(req, "max_turns")->valueint == 40);
   assert(cJSON_GetObjectItem(req, "output") == NULL);
   cJSON_Delete(req);
   printf("  PASS: test_delegate_max_turns_marshaled\n");
}

static void test_delegate_tools_named_toolset_marshaled(void)
{
   char *argv[] = {"execute", "--tools", "readonly",
                   "inspect this repository with read-only tools"};
   cJSON *req = marshal_delegate(4, argv);
   assert(req != NULL);
   assert(strcmp(cJSON_GetObjectItem(req, "role")->valuestring, "execute") == 0);
   assert(strcmp(cJSON_GetObjectItem(req, "prompt")->valuestring,
                 "inspect this repository with read-only tools") == 0);
   assert(cJSON_IsTrue(cJSON_GetObjectItem(req, "tools")));
   assert(strcmp(cJSON_GetObjectItem(req, "toolset")->valuestring, "readonly") == 0);
   cJSON_Delete(req);

   char *eq_argv[] = {"execute", "--tools=readonly",
                      "inspect this repository with read-only tools"};
   req = marshal_delegate(3, eq_argv);
   assert(req != NULL);
   assert(strcmp(cJSON_GetObjectItem(req, "prompt")->valuestring,
                 "inspect this repository with read-only tools") == 0);
   assert(strcmp(cJSON_GetObjectItem(req, "toolset")->valuestring, "readonly") == 0);
   cJSON_Delete(req);

   printf("  PASS: test_delegate_tools_named_toolset_marshaled\n");
}

static void test_delegate_zero_max_turns_marshaled(void)
{
   char *argv[] = {"review", "--max-turns=0", "inspect this bounded diff"};
   cJSON *req = marshal_delegate(3, argv);
   assert(req != NULL);
   assert(cJSON_GetObjectItem(req, "max_turns")->valueint == 0);
   cJSON_Delete(req);
   printf("  PASS: test_delegate_zero_max_turns_marshaled\n");
}

static void test_delegate_provider_model_marshaled(void)
{
   char *argv[] = {"code",    "--provider",       "mistral",
                   "--model", "codestral-latest", "inspect this bounded diff"};
   cJSON *req = marshal_delegate(6, argv);
   assert(req != NULL);
   assert(strcmp(cJSON_GetObjectItem(req, "method")->valuestring, "delegate") == 0);
   assert(strcmp(cJSON_GetObjectItem(req, "role")->valuestring, "code") == 0);
   assert(strcmp(cJSON_GetObjectItem(req, "provider")->valuestring, "mistral") == 0);
   assert(strcmp(cJSON_GetObjectItem(req, "model")->valuestring, "codestral-latest") == 0);
   assert(strcmp(cJSON_GetObjectItem(req, "prompt")->valuestring, "inspect this bounded diff") ==
          0);
   cJSON_Delete(req);
   printf("  PASS: test_delegate_provider_model_marshaled\n");
}

static void test_delegate_persona_marshaled(void)
{
   char *argv[] = {"code", "--persona", "security", "inspect this bounded diff"};
   cJSON *req = marshal_delegate(4, argv);
   assert(req != NULL);
   assert(strcmp(cJSON_GetObjectItem(req, "role")->valuestring, "code") == 0);
   assert(strcmp(cJSON_GetObjectItem(req, "persona")->valuestring, "security") == 0);
   assert(strcmp(cJSON_GetObjectItem(req, "prompt")->valuestring, "inspect this bounded diff") ==
          0);
   cJSON_Delete(req);

   /* No --persona: the field is omitted (the server enforces the requirement). */
   char *argv2[] = {"code", "inspect this bounded diff"};
   req = marshal_delegate(2, argv2);
   assert(req != NULL);
   assert(cJSON_GetObjectItem(req, "persona") == NULL);
   cJSON_Delete(req);
   printf("  PASS: test_delegate_persona_marshaled\n");
}

static void test_delegate_roundtable_brief_marshaled(void)
{
   char *argv[] = {"review this change thoroughly",
                   "--mode",
                   "review",
                   "--turns",
                   "parallel",
                   "--rounds",
                   "2",
                   "--brief",
                   "focus on auth",
                   "--brief-json",
                   "{\"questions\":[\"does auth hold?\"]}",
                   "--apply"};
   cJSON *req = marshal_delegate_roundtable(12, argv);
   assert(req != NULL);
   assert(strcmp(cJSON_GetObjectItem(req, "method")->valuestring, "delegate.roundtable") == 0);
   assert(strcmp(cJSON_GetObjectItem(req, "prompt")->valuestring,
                 "review this change thoroughly") == 0);
   assert(strcmp(cJSON_GetObjectItem(req, "mode")->valuestring, "review") == 0);
   assert(strcmp(cJSON_GetObjectItem(req, "turns")->valuestring, "parallel") == 0);
   assert(cJSON_GetObjectItem(req, "rounds")->valueint == 2);
   assert(cJSON_IsTrue(cJSON_GetObjectItem(req, "apply")));
   cJSON *brief = cJSON_GetObjectItem(req, "brief");
   assert(cJSON_IsObject(brief));
   cJSON *questions = cJSON_GetObjectItem(brief, "questions");
   assert(cJSON_IsArray(questions));
   assert(strcmp(cJSON_GetArrayItem(questions, 0)->valuestring, "does auth hold?") == 0);
   cJSON_Delete(req);
   printf("  PASS: test_delegate_roundtable_brief_marshaled\n");
}

static void test_delegate_roundtable_invalid_brief_json_exits(void)
{
   pid_t pid = fork();
   assert(pid >= 0);
   if (pid == 0)
   {
      char *argv[] = {"review this change thoroughly", "--brief-json", "{\"questions\":["};
      cJSON *req = marshal_delegate_roundtable(3, argv);
      cJSON_Delete(req);
      _exit(0);
   }

   int status = 0;
   assert(waitpid(pid, &status, 0) == pid);
   assert(WIFEXITED(status));
   assert(WEXITSTATUS(status) == 1);
   printf("  PASS: test_delegate_roundtable_invalid_brief_json_exits\n");
}

static cJSON *marshal_delegate_with_stdin(int argc, char **argv, const char *input)
{
   int old_stdin = dup(STDIN_FILENO);
   assert(old_stdin >= 0);
   int pipefd[2];
   assert(pipe(pipefd) == 0);
   if (input)
      assert(write(pipefd[1], input, strlen(input)) == (ssize_t)strlen(input));
   close(pipefd[1]);
   assert(dup2(pipefd[0], STDIN_FILENO) >= 0);
   clearerr(stdin);
   close(pipefd[0]);
   cJSON *req = marshal_delegate(argc, argv);
   assert(dup2(old_stdin, STDIN_FILENO) >= 0);
   clearerr(stdin);
   close(old_stdin);
   return req;
}

static void test_delegate_prompt_stdin_marshaled(void)
{
   char *argv[] = {"review", "--prompt-stdin"};
   cJSON *req = marshal_delegate_with_stdin(2, argv, "inspect `git diff` literally");
   assert(req != NULL);
   assert(strcmp(cJSON_GetObjectItem(req, "method")->valuestring, "delegate") == 0);
   assert(strcmp(cJSON_GetObjectItem(req, "role")->valuestring, "review") == 0);
   assert(strcmp(cJSON_GetObjectItem(req, "prompt")->valuestring, "inspect `git diff` literally") ==
          0);
   cJSON_Delete(req);

   char *tools_argv[] = {"execute", "--tools", "readonly", "--prompt-stdin"};
   req = marshal_delegate_with_stdin(4, tools_argv, "inspect `git diff` literally");
   assert(req != NULL);
   assert(strcmp(cJSON_GetObjectItem(req, "prompt")->valuestring, "inspect `git diff` literally") ==
          0);
   assert(strcmp(cJSON_GetObjectItem(req, "toolset")->valuestring, "readonly") == 0);
   cJSON_Delete(req);
   printf("  PASS: test_delegate_prompt_stdin_marshaled\n");
}

static void test_delegate_status_multiple_ids_marshaled(void)
{
   char *status_argv[] = {"558", "559"};
   cJSON *req = marshal_delegate_status(2, status_argv);
   assert(req != NULL);
   assert(strcmp(cJSON_GetObjectItem(req, "method")->valuestring, "delegate.status") == 0);
   assert(cJSON_GetObjectItem(req, "job_id") == NULL);
   cJSON *job_ids = cJSON_GetObjectItem(req, "job_ids");
   assert(cJSON_IsArray(job_ids));
   assert(cJSON_GetArraySize(job_ids) == 2);
   assert(cJSON_GetArrayItem(job_ids, 0)->valueint == 558);
   assert(cJSON_GetArrayItem(job_ids, 1)->valueint == 559);
   cJSON_Delete(req);

   printf("  PASS: test_delegate_status_multiple_ids_marshaled\n");
}

static void test_delegate_log_rejects_ignored_args(void)
{
   char *log_argv[] = {"558"};
   assert(marshal_delegate_log(1, log_argv) == NULL);

   printf("  PASS: test_delegate_log_rejects_ignored_args\n");
}

static void test_delegate_status_result_options_marshaled(void)
{
   char *full_argv[] = {"558", "--full"};
   cJSON *req = marshal_delegate_status(2, full_argv);
   assert(req != NULL);
   assert(strcmp(cJSON_GetObjectItem(req, "method")->valuestring, "delegate.status") == 0);
   assert(cJSON_GetObjectItem(req, "job_id")->valueint == 558);
   assert(cJSON_IsTrue(cJSON_GetObjectItem(req, "full_result")));
   assert(cJSON_GetObjectItem(req, "result_limit")->valueint == -1);
   cJSON_Delete(req);

   char *limit_argv[] = {"558", "--result-limit", "80"};
   req = marshal_delegate_status(3, limit_argv);
   assert(req != NULL);
   assert(cJSON_GetObjectItem(req, "job_id")->valueint == 558);
   assert(cJSON_GetObjectItem(req, "full_result") == NULL);
   assert(cJSON_GetObjectItem(req, "result_limit")->valueint == 80);
   cJSON_Delete(req);

   printf("  PASS: test_delegate_status_result_options_marshaled\n");
}

static void test_delegate_prompt_stdin_rejects_prompt_file(void)
{
   char *argv[] = {"review", "--prompt-stdin", "--prompt-file", "/tmp/nope"};
   cJSON *req = marshal_delegate_with_stdin(4, argv, "unused");
   assert(req == NULL);
   printf("  PASS: test_delegate_prompt_stdin_rejects_prompt_file\n");
}

static void test_delegate_depth_requires_parent_env(void)
{
   const char *old_depth = getenv("AIMEE_DELEGATE_DEPTH");
   const char *old_parent = getenv("AIMEE_PARENT_DELEGATION_ID");
   char old_depth_buf[32] = {0};
   char old_parent_buf[128] = {0};
   if (old_depth)
      snprintf(old_depth_buf, sizeof(old_depth_buf), "%s", old_depth);
   if (old_parent)
      snprintf(old_parent_buf, sizeof(old_parent_buf), "%s", old_parent);

   setenv("AIMEE_DELEGATE_DEPTH", "3", 1);
   unsetenv("AIMEE_PARENT_DELEGATION_ID");
   char *argv[] = {"review", "inspect this bounded diff"};
   cJSON *req = marshal_delegate(2, argv);
   assert(req != NULL);
   assert(cJSON_GetObjectItem(req, "delegation_depth") == NULL);
   assert(cJSON_GetObjectItem(req, "parent_delegation_id") == NULL);
   cJSON_Delete(req);

   setenv("AIMEE_PARENT_DELEGATION_ID", "deleg-test-parent", 1);
   req = marshal_delegate(2, argv);
   assert(req != NULL);
   assert(cJSON_GetObjectItem(req, "delegation_depth")->valueint == 3);
   assert(strcmp(cJSON_GetObjectItem(req, "parent_delegation_id")->valuestring,
                 "deleg-test-parent") == 0);
   cJSON_Delete(req);
   if (old_depth_buf[0])
      setenv("AIMEE_DELEGATE_DEPTH", old_depth_buf, 1);
   else
      unsetenv("AIMEE_DELEGATE_DEPTH");
   if (old_parent_buf[0])
      setenv("AIMEE_PARENT_DELEGATION_ID", old_parent_buf, 1);
   else
      unsetenv("AIMEE_PARENT_DELEGATION_ID");
   printf("  PASS: test_delegate_depth_requires_parent_env\n");
}

static void test_provider_routes_and_marshaling(void)
{
   cli_v1_route_t route;

   char *list_lookup[] = {"list"};
   assert(cli_v1_lookup("provider", 1, list_lookup, &route));
   assert(strcmp(route.method, "provider.list") == 0);
   assert(route.skip_subcmd == 1);

   char *show_lookup[] = {"show", "mistral"};
   assert(cli_v1_lookup("provider", 2, show_lookup, &route));
   assert(strcmp(route.method, "provider.show") == 0);
   assert(route.skip_subcmd == 1);

   char *models_lookup[] = {"models", "mistral", "--json"};
   assert(cli_v1_lookup("provider", 3, models_lookup, &route));
   assert(strcmp(route.method, "provider.models") == 0);
   assert(route.skip_subcmd == 1);

   char *quota_lookup[] = {"quota", "openrouter"};
   assert(cli_v1_lookup("provider", 2, quota_lookup, &route));
   assert(strcmp(route.method, "provider.quota") == 0);
   assert(route.skip_subcmd == 1);

   char *list_argv[] = {"--available", "--json"};
   cJSON *req = marshal_provider_list(2, list_argv);
   assert(req != NULL);
   assert(strcmp(cJSON_GetObjectItem(req, "method")->valuestring, "provider.list") == 0);
   assert(cJSON_IsTrue(cJSON_GetObjectItem(req, "available_only")));
   assert(cJSON_IsTrue(cJSON_GetObjectItem(req, "json")));
   cJSON_Delete(req);

   char *models_argv[] = {"mistral", "--json"};
   req = marshal_provider_models(2, models_argv);
   assert(req != NULL);
   assert(strcmp(cJSON_GetObjectItem(req, "method")->valuestring, "provider.models") == 0);
   assert(strcmp(cJSON_GetObjectItem(req, "name")->valuestring, "mistral") == 0);
   assert(cJSON_IsTrue(cJSON_GetObjectItem(req, "json")));
   cJSON_Delete(req);

   char *quota_argv[] = {"openrouter"};
   req = marshal_provider_name_method("provider.quota", 1, quota_argv);
   assert(req != NULL);
   assert(strcmp(cJSON_GetObjectItem(req, "method")->valuestring, "provider.quota") == 0);
   assert(strcmp(cJSON_GetObjectItem(req, "name")->valuestring, "openrouter") == 0);
   cJSON_Delete(req);

   printf("  PASS: test_provider_routes_and_marshaling\n");
}

static void test_model_routes_and_marshaling(void)
{
   cli_v1_route_t route;

   char *show_lookup[] = {"show", "openrouter:anthropic/claude-opus-4.6"};
   assert(cli_v1_lookup("model", 2, show_lookup, &route));
   assert(strcmp(route.method, "model.show") == 0);
   assert(route.skip_subcmd == 1);

   char *list_lookup[] = {"list", "--capability", "vision"};
   assert(cli_v1_lookup("model", 3, list_lookup, &route));
   assert(strcmp(route.method, "model.list") == 0);
   assert(route.skip_subcmd == 1);

   cJSON *req = marshal_model_show(1, &show_lookup[1]);
   assert(req != NULL);
   assert(strcmp(cJSON_GetObjectItem(req, "method")->valuestring, "model.show") == 0);
   assert(strcmp(cJSON_GetObjectItem(req, "name")->valuestring,
                 "openrouter:anthropic/claude-opus-4.6") == 0);
   assert(strcmp(cJSON_GetObjectItem(req, "provider")->valuestring, "openrouter") == 0);
   assert(strcmp(cJSON_GetObjectItem(req, "model")->valuestring, "anthropic/claude-opus-4.6") == 0);
   cJSON_Delete(req);

   char *list_argv[] = {"--capability", "vision", "--json", "--open-weights"};
   req = marshal_model_list(4, list_argv);
   assert(req != NULL);
   assert(strcmp(cJSON_GetObjectItem(req, "method")->valuestring, "model.list") == 0);
   assert(strcmp(cJSON_GetObjectItem(req, "capability")->valuestring, "vision") == 0);
   assert(cJSON_IsTrue(cJSON_GetObjectItem(req, "json")));
   assert(cJSON_IsTrue(cJSON_GetObjectItem(req, "open_weights_only")));
   cJSON_Delete(req);

   char *show_argv[] = {"openai:gpt-4o"};
   req = marshal_model_show(1, show_argv);
   assert(req != NULL);
   assert(strcmp(cJSON_GetObjectItem(req, "method")->valuestring, "model.show") == 0);
   assert(strcmp(cJSON_GetObjectItem(req, "name")->valuestring, "openai:gpt-4o") == 0);
   assert(strcmp(cJSON_GetObjectItem(req, "provider")->valuestring, "openai") == 0);
   assert(strcmp(cJSON_GetObjectItem(req, "model")->valuestring, "gpt-4o") == 0);
   cJSON_Delete(req);

   printf("  PASS: test_model_routes_and_marshaling\n");
}

static void test_memory_show_alias_route(void)
{
   cli_v1_route_t route;
   char *show_lookup[] = {"show", "181"};
   assert(cli_v1_lookup("memory", 2, show_lookup, &route));
   assert(strcmp(route.method, "memory.get") == 0);
   assert(route.skip_subcmd == 1);

   char *show_args[] = {"181"};
   cJSON *req = marshal_request(route.method, 1, show_args);
   assert(req != NULL);
   assert(strcmp(cJSON_GetObjectItem(req, "method")->valuestring, "memory.get") == 0);
   assert((long long)cJSON_GetObjectItem(req, "id")->valuedouble == 181);
   cJSON_Delete(req);

   printf("  PASS: test_memory_show_alias_route\n");
}

static void test_memory_stats_route(void)
{
   /* `aimee memory stats` must resolve to a typed server RPC (memory.stats)
    * rather than falling through to unsupported_client_command. Regression
    * guard for the missing route gap from the CLI HTTP transport cutover. */
   cli_v1_route_t route;
   char *stats_lookup[] = {"stats"};
   assert(cli_v1_lookup("memory", 1, stats_lookup, &route));
   assert(strcmp(route.method, "memory.stats") == 0);

   cJSON *req = marshal_request("memory.stats", 0, NULL);
   assert(req != NULL);
   assert(strcmp(cJSON_GetObjectItem(req, "method")->valuestring, "memory.stats") == 0);
   cJSON_Delete(req);

   printf("  PASS: test_memory_stats_route\n");
}

static void test_server_status_route_lookup(void)
{
   cli_v1_route_t route;

   assert(cli_v1_lookup("status", 0, NULL, &route));
   assert(strcmp(route.method, "server.health") == 0);
   assert(route.skip_subcmd == 0);

   char *top_status_json[] = {"--json"};
   assert(cli_v1_lookup("status", 1, top_status_json, &route));
   assert(strcmp(route.method, "server.health") == 0);
   assert(route.skip_subcmd == 0);

   char *status_lookup[] = {"status"};
   assert(cli_v1_lookup("server", 1, status_lookup, &route));
   assert(strcmp(route.method, "server.health") == 0);
   assert(route.skip_subcmd == 1);

   char *health_lookup[] = {"health"};
   assert(cli_v1_lookup("server", 1, health_lookup, &route));
   assert(strcmp(route.method, "server.health") == 0);
   assert(route.skip_subcmd == 1);

   printf("  PASS: test_server_status_route_lookup\n");
}

static void test_kb_docs_push_route_and_marshal(void)
{
   cli_v1_route_t route;
   char *lookup[] = {"docs", "push", "--scope", "project", "a.md", "b.md"};
   assert(cli_v1_lookup("kb", 6, lookup, &route));
   assert(strcmp(route.method, "kb.docs.push") == 0);
   assert(route.skip_subcmd == 2);

   char *args[] = {"--scope", "project", "a.md", "b.md"};
   cJSON *req = marshal_request(route.method, 4, args);
   assert(req != NULL);
   assert(strcmp(cJSON_GetObjectItem(req, "method")->valuestring, "kb.docs.push") == 0);
   assert(strcmp(cJSON_GetObjectItem(req, "scope")->valuestring, "project") == 0);
   cJSON *paths = cJSON_GetObjectItem(req, "paths");
   assert(cJSON_IsArray(paths) && cJSON_GetArraySize(paths) == 2);
   assert(strstr(cJSON_GetArrayItem(paths, 0)->valuestring, "/a.md") != NULL);
   assert(strstr(cJSON_GetArrayItem(paths, 1)->valuestring, "/b.md") != NULL);
   cJSON_Delete(req);

   printf("  PASS: test_kb_docs_push_route_and_marshal\n");
}

static cJSON *mcp_text_response(const char *text)
{
   cJSON *resp = cJSON_CreateObject();
   cJSON_AddStringToObject(resp, "status", "ok");
   cJSON *content = cJSON_AddArrayToObject(resp, "content");
   cJSON *item = cJSON_CreateObject();
   cJSON_AddStringToObject(item, "type", "text");
   cJSON_AddStringToObject(item, "text", text);
   cJSON_AddItemToArray(content, item);
   return resp;
}

static void test_git_verify_failure_detection(void)
{
   cJSON *pass = mcp_text_response("[1/1] lint: PASS (0.1s)\n\nall 1 steps passed");
   assert(!git_verify_response_is_failure(pass));
   cJSON_Delete(pass);

   cJSON *failed_step =
       mcp_text_response("[1/1] unit-tests: FAIL (exit 2, 4.2s)\nfailed test output");
   assert(git_verify_response_is_failure(failed_step));
   cJSON_Delete(failed_step);

   cJSON *failed_summary =
       mcp_text_response("1/4 step(s) failed -- verified with failures (deadbeef)");
   assert(git_verify_response_is_failure(failed_summary));
   cJSON_Delete(failed_summary);

   cJSON *failed_check = mcp_text_response("FAIL: verification failed");
   assert(git_verify_response_is_failure(failed_check));
   cJSON_Delete(failed_check);

   printf("  PASS: test_git_verify_failure_detection\n");
}

static void test_git_verify_marshaled_with_session_id(void)
{
   cli_v1_route_t route;
   char *top_verify_argv[] = {"--async=false"};
   assert(cli_v1_lookup("verify", 1, top_verify_argv, &route));
   assert(strcmp(route.method, "git.verify") == 0);
   assert(strcmp(route.server_method, "mcp.call") == 0);
   assert(route.skip_subcmd == 0);

   char *git_verify_argv[] = {"verify", "--async=false"};
   assert(cli_v1_lookup("git", 2, git_verify_argv, &route));
   assert(strcmp(route.method, "git.verify") == 0);
   assert(strcmp(route.server_method, "mcp.call") == 0);
   assert(route.skip_subcmd == 1);

   const char *old_sid = getenv("AIMEE_SESSION_ID");
   char old_sid_buf[128];
   if (old_sid)
      snprintf(old_sid_buf, sizeof(old_sid_buf), "%s", old_sid);

   setenv("AIMEE_SESSION_ID", "verify-session-test", 1);
   char async_arg[] = "--async=false";
   char *argv[] = {async_arg};
   cJSON *req = marshal_git_verify(1, argv);
   assert(req != NULL);
   assert(strcmp(cJSON_GetObjectItem(req, "method")->valuestring, "mcp.call") == 0);
   assert(strcmp(cJSON_GetObjectItem(req, "tool")->valuestring, "git_verify") == 0);
   assert(strcmp(cJSON_GetObjectItem(req, "session_id")->valuestring, "verify-session-test") == 0);

   cJSON *args = cJSON_GetObjectItem(req, "arguments");
   assert(cJSON_IsObject(args));
   cJSON *async = cJSON_GetObjectItem(args, "async");
   assert(cJSON_IsBool(async));
   assert(cJSON_IsFalse(async));
   cJSON_Delete(req);

   if (old_sid)
      setenv("AIMEE_SESSION_ID", old_sid_buf, 1);
   else
      unsetenv("AIMEE_SESSION_ID");

   printf("  PASS: test_git_verify_marshaled_with_session_id\n");
}

static void test_get_help_route_marshaled(void)
{
   cli_v1_route_t route;
   char *topic_lookup[] = {"work", "queue"};
   assert(cli_v1_lookup("get_help", 2, topic_lookup, &route));
   assert(strcmp(route.method, "get_help") == 0);
   assert(strcmp(route.server_method, "mcp.call") == 0);
   assert(route.skip_subcmd == 0);

   cJSON *req = marshal_request(route.method, 2, topic_lookup);
   assert(req != NULL);
   assert(strcmp(cJSON_GetObjectItem(req, "method")->valuestring, "mcp.call") == 0);
   assert(strcmp(cJSON_GetObjectItem(req, "tool")->valuestring, "get_help") == 0);
   cJSON *args = cJSON_GetObjectItem(req, "arguments");
   assert(cJSON_IsObject(args));
   assert(strcmp(cJSON_GetObjectItem(args, "topic")->valuestring, "work queue") == 0);
   cJSON_Delete(req);

   assert(cli_v1_lookup("get-help", 2, topic_lookup, &route));
   assert(strcmp(route.method, "get_help") == 0);
   assert(strcmp(route.server_method, "mcp.call") == 0);

   req = marshal_request(route.method, 0, NULL);
   assert(req != NULL);
   assert(strcmp(cJSON_GetObjectItem(req, "method")->valuestring, "mcp.call") == 0);
   assert(strcmp(cJSON_GetObjectItem(req, "tool")->valuestring, "get_help") == 0);
   args = cJSON_GetObjectItem(req, "arguments");
   assert(cJSON_IsObject(args));
   assert(cJSON_GetObjectItem(args, "topic") == NULL);
   cJSON_Delete(req);

   printf("  PASS: test_get_help_route_marshaled\n");
}

static void test_subcommand_json_flag_is_output_mode(void)
{
   char *status_argv[] = {"134", "--json"};
   assert(cli_v1_args_request_json(2, status_argv) == 1);

   char *prefix_argv[] = {"--json", "134"};
   assert(cli_v1_args_request_json(2, prefix_argv) == 1);

   char *plain_argv[] = {"134"};
   assert(cli_v1_args_request_json(1, plain_argv) == 0);

   printf("  PASS: test_subcommand_json_flag_is_output_mode\n");
}

static void test_trigger_routes_lookup(void)
{
   cli_v1_route_t route;
   char *list_argv[] = {"list"};
   assert(cli_v1_lookup("trigger", 1, list_argv, &route));
   assert(strcmp(route.method, "trigger.list") == 0);
   assert(route.skip_subcmd == 1);

   char *fire_argv[] = {"fire", "--source", "ci", "--task", "debug failure"};
   assert(cli_v1_lookup("trigger", 5, fire_argv, &route));
   assert(strcmp(route.method, "trigger.fire") == 0);
   assert(route.skip_subcmd == 1);
   printf("  PASS: test_trigger_routes_lookup\n");
}

static void test_dogfood_routes_and_marshaling(void)
{
   cli_v1_route_t route;
   char *review_lookup[] = {"review", "--month", "2026-04"};
   assert(cli_v1_lookup("dogfood", 3, review_lookup, &route));
   assert(strcmp(route.method, "dogfood.review") == 0);
   assert(route.skip_subcmd == 1);
   assert(route.timeout_ms == 300000);

   char *report_lookup[] = {"report", "--month", "2026-04"};
   assert(cli_v1_lookup("dogfood", 3, report_lookup, &route));
   assert(strcmp(route.method, "dogfood.report") == 0);
   assert(route.timeout_ms == 300000);

   char *review_args[] = {"--month", "2026-04", "--dir", "logs", "--limit", "12"};
   cJSON *req = marshal_request("dogfood.review", 6, review_args);
   assert(req != NULL);
   assert(strcmp(cJSON_GetObjectItem(req, "method")->valuestring, "dogfood.review") == 0);
   assert(strcmp(cJSON_GetObjectItem(req, "month")->valuestring, "2026-04") == 0);
   assert(strcmp(cJSON_GetObjectItem(req, "dir")->valuestring, "logs") == 0);
   assert(cJSON_GetObjectItem(req, "limit")->valueint == 12);
   cJSON_Delete(req);

   printf("  PASS: test_dogfood_routes_and_marshaling\n");
}

static void test_eval_routes_and_marshaling(void)
{
   cli_v1_route_t route;
   char *run_lookup[] = {"run", "evals/delegate", "--ablation", "all"};
   assert(cli_v1_lookup("eval", 4, run_lookup, &route));
   assert(strcmp(route.method, "eval.run") == 0);
   assert(route.skip_subcmd == 1);
   assert(route.timeout_ms == 900000);

   char *results_lookup[] = {"results", "delegate-tool-call-reliability"};
   assert(cli_v1_lookup("eval", 2, results_lookup, &route));
   assert(strcmp(route.method, "eval.results") == 0);
   assert(route.timeout_ms == 0);

   char *run_args[] = {"evals/delegate", "--ablation", "no_rescue", "--runs", "3", "--seed", "17"};
   cJSON *req = marshal_request("eval.run", 7, run_args);
   assert(req != NULL);
   assert(strcmp(cJSON_GetObjectItem(req, "method")->valuestring, "eval.run") == 0);
   assert(strcmp(cJSON_GetObjectItem(req, "suite_dir")->valuestring, "evals/delegate") == 0);
   assert(strcmp(cJSON_GetObjectItem(req, "ablation")->valuestring, "no_rescue") == 0);
   assert(cJSON_GetObjectItem(req, "runs")->valueint == 3);
   assert(cJSON_GetObjectItem(req, "seed")->valueint == 17);
   assert(cJSON_IsString(cJSON_GetObjectItem(req, "cwd")));
   cJSON_Delete(req);

   char *results_args[] = {"delegate-tool-call-reliability"};
   req = marshal_request("eval.results", 1, results_args);
   assert(req != NULL);
   assert(strcmp(cJSON_GetObjectItem(req, "method")->valuestring, "eval.results") == 0);
   assert(strcmp(cJSON_GetObjectItem(req, "suite")->valuestring,
                 "delegate-tool-call-reliability") == 0);
   cJSON_Delete(req);

   printf("  PASS: test_eval_routes_and_marshaling\n");
}

static void test_trigger_fire_token_marshaled(void)
{
   char *argv[] = {"--source", "github",    "--event",     "pull_request.opened",
                   "--task",   "Review PR", "--workspace", "aimee",
                   "--token",  "secret"};
   cJSON *req = marshal_trigger_fire(10, argv);
   assert(req != NULL);
   assert(strcmp(cJSON_GetObjectItem(req, "method")->valuestring, "trigger.fire") == 0);
   assert(strcmp(cJSON_GetObjectItem(req, "source")->valuestring, "github") == 0);
   assert(strcmp(cJSON_GetObjectItem(req, "event")->valuestring, "pull_request.opened") == 0);
   assert(strcmp(cJSON_GetObjectItem(req, "task")->valuestring, "Review PR") == 0);
   assert(strcmp(cJSON_GetObjectItem(req, "workspace")->valuestring, "aimee") == 0);
   assert(strcmp(cJSON_GetObjectItem(req, "auth_token")->valuestring, "secret") == 0);
   assert(cJSON_GetObjectItem(req, "token") == NULL);
   cJSON_Delete(req);
   printf("  PASS: test_trigger_fire_token_marshaled\n");
}

static void test_cron_routes_and_marshaling(void)
{
   cli_v1_route_t route;
   char *add_lookup[] = {"add"};
   assert(cli_v1_lookup("cron", 1, add_lookup, &route));
   assert(strcmp(route.method, "cron.add") == 0);
   assert(route.skip_subcmd == 1);

   char *add_argv[] = {
       "pve-pulse",          "--schedule", "every 10m", "--mode", "script",
       "--script",           "echo OK",    "--target",  "local",  "--only-if-changed",
       "--first-run-silent", "--skill",    "kb-health"};
   cJSON *req = marshal_cron_add(13, add_argv);
   assert(req != NULL);
   assert(strcmp(cJSON_GetObjectItem(req, "method")->valuestring, "cron.add") == 0);
   assert(strcmp(cJSON_GetObjectItem(req, "job_id")->valuestring, "pve-pulse") == 0);
   assert(strcmp(cJSON_GetObjectItem(req, "schedule")->valuestring, "every 10m") == 0);
   assert(strcmp(cJSON_GetObjectItem(req, "mode")->valuestring, "script") == 0);
   assert(strcmp(cJSON_GetObjectItem(req, "script")->valuestring, "echo OK") == 0);
   assert(strcmp(cJSON_GetObjectItem(req, "deliver_target")->valuestring, "local") == 0);
   assert(cJSON_IsTrue(cJSON_GetObjectItem(req, "deliver_only_if_changed")));
   assert(cJSON_IsTrue(cJSON_GetObjectItem(req, "deliver_first_run_silent")));
   cJSON *skills = cJSON_GetObjectItem(req, "skills");
   assert(cJSON_IsArray(skills));
   assert(cJSON_GetArraySize(skills) == 1);
   assert(strcmp(cJSON_GetArrayItem(skills, 0)->valuestring, "kb-health") == 0);
   cJSON_Delete(req);

   char *disable_argv[] = {"pve-pulse"};
   req = marshal_request("cron.disable", 1, disable_argv);
   assert(req != NULL);
   assert(strcmp(cJSON_GetObjectItem(req, "method")->valuestring, "cron.disable") == 0);
   assert(strcmp(cJSON_GetObjectItem(req, "job_id")->valuestring, "pve-pulse") == 0);
   cJSON_Delete(req);

   char *disable_all_argv[] = {"--all"};
   req = marshal_request("cron.disable", 1, disable_all_argv);
   assert(req != NULL);
   assert(strcmp(cJSON_GetObjectItem(req, "method")->valuestring, "cron.disable") == 0);
   assert(cJSON_IsTrue(cJSON_GetObjectItem(req, "all")));
   cJSON_Delete(req);

   printf("  PASS: test_cron_routes_and_marshaling\n");
}

static void test_work_routes_lookup(void)
{
   cli_v1_route_t route;
   char *claim_argv[] = {"claim"};
   assert(cli_v1_lookup("work", 1, claim_argv, &route));
   assert(strcmp(route.method, "work.claim") == 0);
   assert(route.skip_subcmd == 1);

   char *sync_argv[] = {"sync-proposals"};
   assert(cli_v1_lookup("work", 1, sync_argv, &route));
   assert(strcmp(route.method, "work.sync_proposals") == 0);
   assert(route.skip_subcmd == 1);
   printf("  PASS: test_work_routes_lookup\n");
}

static void test_session_brief_route_marshaled(void)
{
   cli_v1_route_t route;
   char *lookup[] = {"brief", "--session", "abc123", "--list"};
   assert(cli_v1_lookup("session", 4, lookup, &route));
   assert(strcmp(route.method, "session.brief") == 0);
   assert(route.skip_subcmd == 1);

   cJSON *req = marshal_request("session.brief", 3, lookup + 1);
   assert(req != NULL);
   assert(strcmp(cJSON_GetObjectItem(req, "method")->valuestring, "session.brief") == 0);
   assert(strcmp(cJSON_GetObjectItem(req, "session_id")->valuestring, "abc123") == 0);
   assert(cJSON_IsTrue(cJSON_GetObjectItem(req, "list")));
   cJSON_Delete(req);

   char *pos_lookup[] = {"brief", "def456"};
   assert(cli_v1_lookup("session", 2, pos_lookup, &route));
   req = marshal_request("session.brief", 1, pos_lookup + 1);
   assert(req != NULL);
   assert(strcmp(cJSON_GetObjectItem(req, "session_id")->valuestring, "def456") == 0);
   cJSON_Delete(req);

   printf("  PASS: test_session_brief_route_marshaled\n");
}

static void test_work_claim_marshaled(void)
{
   unsetenv("AIMEE_SESSION_ID");
   unsetenv("CLAUDE_SESSION_ID");
   char *argv[] = {"--effort", "M", "--tag", "server", "--exclude-tag", "blocked", "--skip", "2"};
   cJSON *req = marshal_work_request("work.claim", 8, argv);
   assert(req != NULL);
   assert(strcmp(cJSON_GetObjectItem(req, "method")->valuestring, "work.claim") == 0);
   assert(strcmp(cJSON_GetObjectItem(req, "effort")->valuestring, "M") == 0);
   assert(strcmp(cJSON_GetObjectItem(req, "tag")->valuestring, "server") == 0);
   assert(strcmp(cJSON_GetObjectItem(req, "exclude_tag")->valuestring, "blocked") == 0);
   assert(cJSON_GetObjectItem(req, "skip")->valueint == 2);
   assert(strcmp(cJSON_GetObjectItem(req, "session_id")->valuestring, "default") == 0);
   cJSON_Delete(req);
   printf("  PASS: test_work_claim_marshaled\n");
}

static void test_work_add_batch_marshaled(void)
{
   char *argv[] = {"--from-proposals", "--dir", "docs/proposals/pending"};
   cJSON *req = marshal_work_request("work.add_batch", 3, argv);
   assert(req != NULL);
   assert(strcmp(cJSON_GetObjectItem(req, "method")->valuestring, "work.add_batch") == 0);
   assert(cJSON_IsTrue(cJSON_GetObjectItem(req, "from_proposals")));
   assert(strcmp(cJSON_GetObjectItem(req, "dir")->valuestring, "docs/proposals/pending") == 0);
   assert(cJSON_IsString(cJSON_GetObjectItem(req, "client_cwd")));
   cJSON_Delete(req);
   printf("  PASS: test_work_add_batch_marshaled\n");
}

static void test_jobs_routes_lookup(void)
{
   cli_v1_route_t route;
   char *list_argv[] = {"list"};
   assert(cli_v1_lookup("jobs", 1, list_argv, &route));
   assert(strcmp(route.method, "jobs.list") == 0);
   assert(route.skip_subcmd == 1);

   char *status_argv[] = {"status", "12"};
   assert(cli_v1_lookup("jobs", 2, status_argv, &route));
   assert(strcmp(route.method, "jobs.status") == 0);
   assert(route.skip_subcmd == 1);

   char *show_argv[] = {"show", "12"};
   assert(cli_v1_lookup("jobs", 2, show_argv, &route));
   assert(strcmp(route.method, "jobs.status") == 0);
   assert(route.skip_subcmd == 1);

   char *logs_argv[] = {"logs", "12"};
   assert(cli_v1_lookup("jobs", 2, logs_argv, &route));
   assert(strcmp(route.method, "jobs.logs") == 0);
   assert(route.skip_subcmd == 1);

   char *cancel_argv[] = {"cancel", "12"};
   assert(cli_v1_lookup("jobs", 2, cancel_argv, &route));
   assert(strcmp(route.method, "jobs.cancel") == 0);
   assert(route.skip_subcmd == 1);
   printf("  PASS: test_jobs_routes_lookup\n");
}

static void test_jobs_requests_marshaled(void)
{
   char *list_argv[] = {"--limit", "7"};
   cJSON *req = marshal_jobs_list(2, list_argv);
   assert(req != NULL);
   assert(strcmp(cJSON_GetObjectItem(req, "method")->valuestring, "jobs.list") == 0);
   assert(cJSON_GetObjectItem(req, "limit")->valueint == 7);
   cJSON_Delete(req);

   char *status_argv[] = {"42"};
   req = marshal_job_id_request("jobs.status", 1, status_argv);
   assert(req != NULL);
   assert(strcmp(cJSON_GetObjectItem(req, "method")->valuestring, "jobs.status") == 0);
   assert(cJSON_GetObjectItem(req, "job_id")->valueint == 42);
   cJSON_Delete(req);

   char *logs_argv[] = {"44"};
   req = marshal_job_id_request("jobs.logs", 1, logs_argv);
   assert(req != NULL);
   assert(strcmp(cJSON_GetObjectItem(req, "method")->valuestring, "jobs.logs") == 0);
   assert(cJSON_GetObjectItem(req, "job_id")->valueint == 44);
   cJSON_Delete(req);

   char *cancel_argv[] = {"43", "--reason", "operator requested"};
   req = marshal_job_id_request("jobs.cancel", 3, cancel_argv);
   assert(req != NULL);
   assert(strcmp(cJSON_GetObjectItem(req, "method")->valuestring, "jobs.cancel") == 0);
   assert(cJSON_GetObjectItem(req, "job_id")->valueint == 43);
   assert(strcmp(cJSON_GetObjectItem(req, "reason")->valuestring, "operator requested") == 0);
   cJSON_Delete(req);
   printf("  PASS: test_jobs_requests_marshaled\n");
}

static void test_coord_job_routes_lookup(void)
{
   cli_v1_route_t route;
   char *start_argv[] = {"start", "7"};
   assert(cli_v1_lookup("job", 2, start_argv, &route));
   assert(strcmp(route.method, "job.start") == 0);
   assert(route.skip_subcmd == 1);

   char *list_argv[] = {"list"};
   assert(cli_v1_lookup("job", 1, list_argv, &route));
   assert(strcmp(route.method, "job.list") == 0);
   assert(route.skip_subcmd == 1);

   char *status_argv[] = {"status", "12"};
   assert(cli_v1_lookup("job", 2, status_argv, &route));
   assert(strcmp(route.method, "job.status") == 0);
   assert(route.skip_subcmd == 1);

   char *show_argv[] = {"show", "12"};
   assert(cli_v1_lookup("job", 2, show_argv, &route));
   assert(strcmp(route.method, "job.status") == 0);
   assert(route.skip_subcmd == 1);

   char *cancel_argv[] = {"cancel", "12"};
   assert(cli_v1_lookup("job", 2, cancel_argv, &route));
   assert(strcmp(route.method, "job.cancel") == 0);
   assert(route.skip_subcmd == 1);
   printf("  PASS: test_coord_job_routes_lookup\n");
}

static void test_coord_job_requests_marshaled(void)
{
   char *start_argv[] = {"9", "--parallel", "4"};
   cJSON *req = marshal_coord_job_start(3, start_argv);
   assert(req != NULL);
   assert(strcmp(cJSON_GetObjectItem(req, "method")->valuestring, "job.start") == 0);
   assert(cJSON_GetObjectItem(req, "plan_id")->valueint == 9);
   assert(cJSON_GetObjectItem(req, "parallel")->valueint == 4);
   cJSON_Delete(req);

   char *list_argv[] = {"--limit", "3"};
   req = marshal_coord_jobs_list(2, list_argv);
   assert(req != NULL);
   assert(strcmp(cJSON_GetObjectItem(req, "method")->valuestring, "job.list") == 0);
   assert(cJSON_GetObjectItem(req, "limit")->valueint == 3);
   cJSON_Delete(req);

   char *status_argv[] = {"44"};
   req = marshal_job_id_request("job.status", 1, status_argv);
   assert(req != NULL);
   assert(strcmp(cJSON_GetObjectItem(req, "method")->valuestring, "job.status") == 0);
   assert(cJSON_GetObjectItem(req, "job_id")->valueint == 44);
   cJSON_Delete(req);

   printf("  PASS: test_coord_job_requests_marshaled\n");
}

static void test_aux_routes_lookup(void)
{
   cli_v1_route_t route;
   char *config_argv[] = {"config"};
   assert(cli_v1_lookup("aux", 1, config_argv, &route));
   assert(strcmp(route.method, "aux.config_show") == 0);
   assert(route.skip_subcmd == 1);

   char *show_argv[] = {"config", "show"};
   assert(cli_v1_lookup("aux", 2, show_argv, &route));
   assert(strcmp(route.method, "aux.config_show") == 0);
   assert(route.skip_subcmd == 2);

   char *test_argv[] = {"test", "title", "summarize this"};
   assert(cli_v1_lookup("aux", 3, test_argv, &route));
   assert(strcmp(route.method, "aux.test") == 0);
   assert(route.skip_subcmd == 1);
   printf("  PASS: test_aux_routes_lookup\n");
}

static void test_aux_test_marshaled(void)
{
   char *argv[] = {"title", "summarize this", "128"};
   cJSON *req = marshal_aux_test(3, argv);
   assert(req != NULL);
   assert(strcmp(cJSON_GetObjectItem(req, "method")->valuestring, "aux.test") == 0);
   assert(strcmp(cJSON_GetObjectItem(req, "task")->valuestring, "title") == 0);
   assert(strcmp(cJSON_GetObjectItem(req, "prompt")->valuestring, "summarize this") == 0);
   assert(cJSON_GetObjectItem(req, "max_tokens")->valueint == 128);
   cJSON_Delete(req);
   printf("  PASS: test_aux_test_marshaled\n");
}

static void test_mcp_routes_lookup(void)
{
   cli_v1_route_t route;
   char *audit_argv[] = {"audit"};
   assert(cli_v1_lookup("mcp", 1, audit_argv, &route));
   assert(strcmp(route.method, "mcp.audit") == 0);
   assert(route.skip_subcmd == 1);

   char *recheck_argv[] = {"recheck", "server"};
   assert(cli_v1_lookup("mcp", 2, recheck_argv, &route));
   assert(strcmp(route.method, "mcp.recheck") == 0);
   assert(route.skip_subcmd == 1);

   cJSON *req = marshal_request(route.method, 1, recheck_argv + 1);
   assert(req != NULL);
   assert(strcmp(cJSON_GetObjectItem(req, "method")->valuestring, "mcp.recheck") == 0);
   assert(strcmp(cJSON_GetObjectItem(req, "name")->valuestring, "server") == 0);
   cJSON_Delete(req);
   printf("  PASS: test_mcp_routes_lookup\n");
}

static void test_insights_text_output(void)
{
   cli_v1_route_t route;
   assert(cli_v1_lookup("insights", 0, NULL, &route));
   assert(strcmp(route.method, "insights.overview") == 0);

   char *argv[] = {"--days", "7"};
   cJSON *req = marshal_request(route.method, 2, argv);
   assert(req != NULL);
   assert(strcmp(cJSON_GetObjectItem(req, "method")->valuestring, "insights.overview") == 0);
   assert(cJSON_GetObjectItem(req, "days")->valueint == 7);
   cJSON_Delete(req);

   cJSON *resp = cJSON_CreateObject();
   cJSON_AddStringToObject(resp, "status", "ok");
   cJSON_AddNumberToObject(resp, "days", 7);
   cJSON_AddNumberToObject(resp, "total_calls", 3);
   cJSON_AddNumberToObject(resp, "prompt_tokens", 100);
   cJSON_AddNumberToObject(resp, "completion_tokens", 20);
   cJSON_AddNumberToObject(resp, "cache_write_tokens", 0);
   cJSON_AddNumberToObject(resp, "cache_read_tokens", 0);
   cJSON_AddNumberToObject(resp, "estimated_cost_usd", 0.25);
   cJSON_AddItemToObject(resp, "models", cJSON_CreateArray());

   char path[] = "/tmp/aimee-insights-output-XXXXXX";
   int fd = mkstemp(path);
   assert(fd >= 0);
   int old_stdout = dup(STDOUT_FILENO);
   assert(old_stdout >= 0);
   fflush(stdout);
   assert(dup2(fd, STDOUT_FILENO) >= 0);
   print_text_output(route.method, resp);
   fflush(stdout);
   assert(dup2(old_stdout, STDOUT_FILENO) >= 0);
   close(old_stdout);
   close(fd);

   FILE *f = fopen(path, "rb");
   assert(f != NULL);
   char buf[512];
   size_t n = fread(buf, 1, sizeof(buf) - 1, f);
   fclose(f);
   unlink(path);
   buf[n] = '\0';
   assert(strstr(buf, "Insights") != NULL);
   assert(strstr(buf, "last 7 days") != NULL);
   assert(strstr(buf, "calls:") != NULL);
   assert(strstr(buf, "Models: (none)") != NULL);
   cJSON_Delete(resp);
   printf("  PASS: test_insights_text_output\n");
}

static void test_skill_lint_route_marshaled(void)
{
   cli_v1_route_t route;
   char *all_argv[] = {"lint", "--all"};
   assert(cli_v1_lookup("skill", 2, all_argv, &route));
   assert(strcmp(route.method, "skill.lint") == 0);
   cJSON *req = marshal_request(route.method, 1, all_argv + 1);
   assert(req != NULL);
   assert(strcmp(cJSON_GetObjectItem(req, "method")->valuestring, "skill.lint") == 0);
   assert(cJSON_IsTrue(cJSON_GetObjectItem(req, "all")));
   cJSON_Delete(req);

   char *one_argv[] = {"lint", "writing-skills"};
   assert(cli_v1_lookup("skill", 2, one_argv, &route));
   req = marshal_request(route.method, 1, one_argv + 1);
   assert(req != NULL);
   assert(strcmp(cJSON_GetObjectItem(req, "name")->valuestring, "writing-skills") == 0);
   cJSON_Delete(req);
   printf("  PASS: test_skill_lint_route_marshaled\n");
}

static void test_skill_eval_route_marshaled(void)
{
   cli_v1_route_t route;
   char *argv[] = {"eval", "verification-before-completion", "--json"};
   assert(cli_v1_lookup("skill", 3, argv, &route));
   assert(strcmp(route.method, "skill.eval") == 0);
   cJSON *req = marshal_request(route.method, 2, argv + 1);
   assert(req != NULL);
   assert(strcmp(cJSON_GetObjectItem(req, "method")->valuestring, "skill.eval") == 0);
   assert(strcmp(cJSON_GetObjectItem(req, "name")->valuestring, "verification-before-completion") ==
          0);
   cJSON_Delete(req);
   printf("  PASS: test_skill_eval_route_marshaled\n");
}

static void test_skill_autostub_route_marshaled(void)
{
   cli_v1_route_t route;
   char *argv[] = {"autostub", "--force", "--snapshot", "/tmp/tools.json"};
   assert(cli_v1_lookup("skill", 4, argv, &route));
   assert(strcmp(route.method, "skill.autostub") == 0);
   cJSON *req = marshal_request(route.method, 3, argv + 1);
   assert(req != NULL);
   assert(strcmp(cJSON_GetObjectItem(req, "method")->valuestring, "skill.autostub") == 0);
   assert(cJSON_IsTrue(cJSON_GetObjectItem(req, "force")));
   assert(strcmp(cJSON_GetObjectItem(req, "snapshot_path")->valuestring, "/tmp/tools.json") == 0);
   cJSON_Delete(req);
   printf("  PASS: test_skill_autostub_route_marshaled\n");
}

static void test_trajectory_export_route_marshaled(void)
{
   cli_v1_route_t route;
   char *argv[] = {"export", "sess-123", "--no-compress", "--max-result-bytes", "64"};
   assert(cli_v1_lookup("trajectory", 5, argv, &route));
   assert(strcmp(route.method, "trajectory.export") == 0);
   assert(route.skip_subcmd == 1);

   cJSON *req = marshal_request(route.method, 4, argv + 1);
   assert(req != NULL);
   assert(strcmp(cJSON_GetObjectItem(req, "method")->valuestring, "trajectory.export") == 0);
   assert(strcmp(cJSON_GetObjectItem(req, "session_id")->valuestring, "sess-123") == 0);
   assert(cJSON_IsFalse(cJSON_GetObjectItem(req, "compress")));
   assert(cJSON_GetObjectItem(req, "max_result_bytes")->valueint == 64);
   cJSON_Delete(req);

   printf("  PASS: test_trajectory_export_route_marshaled\n");
}

static void test_trajectory_batch_route_marshaled(void)
{
   cli_v1_route_t route;
   char *argv[] = {"batch", "--tasks", "/tmp/corpus.jsonl", "--toolset-dist",
                   "mixed", "--out",   "/tmp/traj"};
   assert(cli_v1_lookup("trajectory", 7, argv, &route));
   assert(strcmp(route.method, "trajectory.batch") == 0);
   assert(route.skip_subcmd == 1);

   cJSON *req = marshal_request(route.method, 6, argv + 1);
   assert(req != NULL);
   assert(strcmp(cJSON_GetObjectItem(req, "method")->valuestring, "trajectory.batch") == 0);
   assert(strcmp(cJSON_GetObjectItem(req, "tasks_path")->valuestring, "/tmp/corpus.jsonl") == 0);
   assert(strcmp(cJSON_GetObjectItem(req, "toolset_dist")->valuestring, "mixed") == 0);
   assert(strcmp(cJSON_GetObjectItem(req, "out_dir")->valuestring, "/tmp/traj") == 0);
   cJSON_Delete(req);

   printf("  PASS: test_trajectory_batch_route_marshaled\n");
}

/* cli_v1_client_endpoint()/cli_v1_client_bearer() (in cli_v1_routes.inc) call
 * aimee_home(), defined in posix/cli_client.c — which cannot be co-linked here
 * because it re-includes the same .inc. Stub it: aimee_home() honors the
 * AIMEE_HOME override exactly as the real one does. (The legacy
 * client_transport selection was removed with the NDJSON transport; the thin
 * client is now an unconditional /v1 consumer.) */
const char *aimee_home(void)
{
   return getenv("AIMEE_HOME");
}

/* cli_v1_client_endpoint()/cli_v1_client_bearer() resolve the remote /v1
 * endpoint + bearer (AIMEE_API_ENDPOINT / AIMEE_API_BEARER env, else aimee.yaml
 * client_endpoint / bearer_token). The thin client is now an unconditional /v1
 * consumer, so cli_v1_has_remote_endpoint() is true exactly when an endpoint is
 * configured (the legacy client_transport gate was removed). */
static void test_client_endpoint_selection(void)
{
   unsetenv("AIMEE_API_ENDPOINT");
   unsetenv("AIMEE_API_BEARER");

   char home[] = "/tmp/aimee-rpce-XXXXXX";
   assert(mkdtemp(home) != NULL);
   setenv("AIMEE_HOME", home, 1);
   unsetenv("AIMEE_PROFILE");

   /* Nothing configured -> no endpoint, no bearer, no remote. */
   assert(cli_v1_client_endpoint() == NULL);
   assert(cli_v1_client_bearer() == NULL);
   assert(cli_v1_has_remote_endpoint() == 0);

   /* Env override wins for both endpoint and bearer; a configured endpoint makes
    * has_remote true on its own. */
   setenv("AIMEE_API_ENDPOINT", "tcp:10.0.0.5:8740", 1);
   setenv("AIMEE_API_BEARER", "env-token", 1);
   char *ep = cli_v1_client_endpoint();
   assert(ep && strcmp(ep, "tcp:10.0.0.5:8740") == 0);
   free(ep);
   char *bt = cli_v1_client_bearer();
   assert(bt && strcmp(bt, "env-token") == 0);
   free(bt);
   assert(cli_v1_has_remote_endpoint() == 1);

   unsetenv("AIMEE_API_ENDPOINT");
   unsetenv("AIMEE_API_BEARER");

   /* aimee.yaml fallback: client_endpoint + bearer_token are read by scan. */
   char yaml[256];
   snprintf(yaml, sizeof(yaml), "%s/aimee.yaml", home);
   FILE *fp = fopen(yaml, "w");
   assert(fp != NULL);
   fputs("aimee:\n  api:\n    client_endpoint: tcp:host.example:8740\n"
         "    bearer_token: \"yaml-token\"\n",
         fp);
   fclose(fp);
   ep = cli_v1_client_endpoint();
   assert(ep && strcmp(ep, "tcp:host.example:8740") == 0);
   free(ep);
   bt = cli_v1_client_bearer();
   assert(bt && strcmp(bt, "yaml-token") == 0);
   free(bt);

   /* yaml endpoint -> remote. */
   assert(cli_v1_has_remote_endpoint() == 1);

   unlink(yaml);
   rmdir(home);
   unsetenv("AIMEE_HOME");
   printf("  PASS: test_client_endpoint_selection\n");
}

/* The thin client must fold --context-file / --files preloads into the prompt,
 * because the server delegate handler only reads `prompt`. Regression guard for
 * the bug where these advertised flags were silently dropped. */
static void test_delegate_context_file_folded_into_prompt(void)
{
   char path[] = "/tmp/aimee_ctx_test_XXXXXX";
   int fd = mkstemp(path);
   assert(fd >= 0);
   const char *marker = "UNIQUE_PRELOAD_MARKER_42 token_normalize_xyz";
   assert(write(fd, marker, strlen(marker)) == (ssize_t)strlen(marker));
   close(fd);

   char *argv[] = {"code", "--context-file", path, "implement the change described above"};
   cJSON *req = marshal_delegate(4, argv);
   assert(req != NULL);
   const cJSON *prompt = cJSON_GetObjectItem(req, "prompt");
   assert(cJSON_IsString(prompt));
   /* Original prompt text preserved... */
   assert(strstr(prompt->valuestring, "implement the change described above") != NULL);
   /* ...and the file contents are injected. */
   assert(strstr(prompt->valuestring, "UNIQUE_PRELOAD_MARKER_42") != NULL);
   assert(strstr(prompt->valuestring, "Source Packet: Preloaded Context") != NULL);
   assert(strstr(prompt->valuestring, path) != NULL);
   cJSON_Delete(req);

   /* --files (comma-separated) is the same mechanism. */
   char files_flag[256];
   snprintf(files_flag, sizeof(files_flag), "%s", path);
   char *argv2[] = {"code", "--files", files_flag, "do the work using the preloaded files"};
   cJSON *req2 = marshal_delegate(4, argv2);
   assert(req2 != NULL);
   const cJSON *p2 = cJSON_GetObjectItem(req2, "prompt");
   assert(cJSON_IsString(p2));
   assert(strstr(p2->valuestring, "UNIQUE_PRELOAD_MARKER_42") != NULL);
   cJSON_Delete(req2);

   /* No preload flags => prompt is unchanged (no spurious Source Packet). */
   char *argv3[] = {"code", "a plain prompt with no preload flags at all"};
   cJSON *req3 = marshal_delegate(2, argv3);
   assert(req3 != NULL);
   const cJSON *p3 = cJSON_GetObjectItem(req3, "prompt");
   assert(cJSON_IsString(p3));
   assert(strstr(p3->valuestring, "Source Packet: Preloaded Context") == NULL);
   cJSON_Delete(req3);

   unlink(path);
   printf("  PASS: test_delegate_context_file_folded_into_prompt\n");
}

int main(void)
{
   printf("test_cli_v1_delegate\n");
   test_delegate_context_file_folded_into_prompt();
   test_delegate_max_turns_marshaled();
   test_delegate_tools_named_toolset_marshaled();
   test_delegate_zero_max_turns_marshaled();
   test_delegate_provider_model_marshaled();
   test_delegate_persona_marshaled();
   test_delegate_roundtable_brief_marshaled();
   test_delegate_roundtable_invalid_brief_json_exits();
   test_delegate_prompt_stdin_marshaled();
   test_delegate_status_multiple_ids_marshaled();
   test_delegate_log_rejects_ignored_args();
   test_delegate_status_result_options_marshaled();
   test_delegate_prompt_stdin_rejects_prompt_file();
   test_delegate_depth_requires_parent_env();
   test_provider_routes_and_marshaling();
   test_model_routes_and_marshaling();
   test_memory_show_alias_route();
   test_memory_stats_route();
   test_server_status_route_lookup();
   test_kb_docs_push_route_and_marshal();
   test_git_verify_failure_detection();
   test_git_verify_marshaled_with_session_id();
   test_get_help_route_marshaled();
   test_subcommand_json_flag_is_output_mode();
   test_trigger_routes_lookup();
   test_dogfood_routes_and_marshaling();
   test_eval_routes_and_marshaling();
   test_trigger_fire_token_marshaled();
   test_cron_routes_and_marshaling();
   test_work_routes_lookup();
   test_session_brief_route_marshaled();
   test_work_claim_marshaled();
   test_work_add_batch_marshaled();
   test_jobs_routes_lookup();
   test_jobs_requests_marshaled();
   test_coord_job_routes_lookup();
   test_coord_job_requests_marshaled();
   test_aux_routes_lookup();
   test_aux_test_marshaled();
   test_mcp_routes_lookup();
   test_insights_text_output();
   test_skill_lint_route_marshaled();
   test_skill_eval_route_marshaled();
   test_skill_autostub_route_marshaled();
   test_trajectory_export_route_marshaled();
   test_trajectory_batch_route_marshaled();
   test_client_endpoint_selection();
   printf("All tests passed.\n");
   return 0;
}
