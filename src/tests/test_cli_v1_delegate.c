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
#include "server.h" /* SHTTP_MAX_BODY: the cap the CLI mirrors */

#define V1_PROTOCOL_VERSION 1

/* Include the route implementation directly so static marshal helpers are
 * testable. It was one .inc; it is now four sibling TUs, pulled in together. */
#include "../cli_v1_routes.c"
#include "../cli_v1_routes_b.c"
#include "../cli_v1_routes_c.c"
#include "../cli_v1_routes_d.c"
#include "../cli_v1_routes_e.c"
#include "platform_test_util.h" /* platform_tmpdir: honour TMPDIR, do not leak into /tmp */

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

static void test_json_error_envelopes_remain_structured(void)
{
   cJSON *deadline =
       cJSON_Parse("{\"ok\":false,\"error\":\"panel deadline\",\"roundtable\":{"
                   "\"participants_failed\":2,\"participant_failures\":[{\"seat\":1}]}}");
   cJSON *dispatch = cJSON_Parse("{\"status\":\"error\",\"message\":\"queue full\"}");
   cJSON *object_error =
       cJSON_Parse("{\"error\":{\"message\":\"forbidden\",\"type\":\"permission_error\"}}");
   cJSON *success = cJSON_Parse("{\"ok\":true,\"result\":{}}");
   assert(deadline && dispatch && object_error && success);

   assert(cli_v1_response_is_error(deadline, 503) == 1);
   assert(cli_v1_response_is_error(dispatch, 200) == 1);
   assert(cli_v1_response_is_error(object_error, 200) == 1);
   assert(cli_v1_response_is_error(success, 200) == 0);

   cJSON_Delete(deadline);
   cJSON_Delete(dispatch);
   cJSON_Delete(object_error);
   cJSON_Delete(success);
   printf("  PASS: test_json_error_envelopes_remain_structured\n");
}

/* Capture the exact post-transport production path, including ownership,
 * stream selection, JSON shaping, and process return code. */
static int finish_response_capture(const char *method, const char *json, int http_status,
                                   int json_output, char *out, size_t out_cap, char *err,
                                   size_t err_cap)
{
   cJSON *resp = cJSON_Parse(json);
   assert(resp != NULL);
   cli_v1_route_t route = {.method = method};

   FILE *out_file = tmpfile(), *err_file = tmpfile();
   assert(out_file != NULL && err_file != NULL);
   int ofd = fileno(out_file), efd = fileno(err_file);
   int old_out = dup(STDOUT_FILENO), old_err = dup(STDERR_FILENO);
   assert(old_out >= 0 && old_err >= 0);
   fflush(stdout);
   fflush(stderr);
   assert(dup2(ofd, STDOUT_FILENO) >= 0);
   assert(dup2(efd, STDERR_FILENO) >= 0);

   int rc = cli_v1_finish_response(&route, resp, http_status, json_output, 0, NULL);

   fflush(stdout);
   fflush(stderr);
   assert(dup2(old_out, STDOUT_FILENO) >= 0);
   assert(dup2(old_err, STDERR_FILENO) >= 0);
   close(old_out);
   close(old_err);

   rewind(out_file);
   size_t n = fread(out, 1, out_cap - 1, out_file);
   out[n] = '\0';
   rewind(err_file);
   n = fread(err, 1, err_cap - 1, err_file);
   err[n] = '\0';
   fclose(out_file);
   fclose(err_file);
   return rc;
}

static void test_json_roundtable_failure_preserves_user_visible_envelope(void)
{
   char out[4096], err[2048];
   int rc = finish_response_capture(
       "roundtable.review",
       "{\"status\":\"error\",\"error\":\"panel deadline\",\"message\":\"panel deadline\","
       "\"roundtable\":{\"deadline_hit\":true,\"participant_failures\":[{\"seat\":1,"
       "\"category\":\"deadline\"},{\"seat\":2,\"category\":\"deadline\"}]}}",
       503, 1, out, sizeof(out), err, sizeof(err));

   assert(rc == 1);
   assert(err[0] == '\0');
   cJSON *printed = cJSON_Parse(out);
   assert(printed != NULL);
   assert(strcmp(cJSON_GetObjectItem(printed, "status")->valuestring, "error") == 0);
   cJSON *roundtable = cJSON_GetObjectItem(printed, "roundtable");
   assert(cJSON_IsTrue(cJSON_GetObjectItem(roundtable, "deadline_hit")));
   assert(cJSON_GetArraySize(cJSON_GetObjectItem(roundtable, "participant_failures")) == 2);
   cJSON_Delete(printed);
   printf("  PASS: test_json_roundtable_failure_preserves_user_visible_envelope\n");
}

static void test_human_roundtable_failure_uses_stderr(void)
{
   char out[2048], err[2048];
   int rc = finish_response_capture(
       "roundtable.review",
       "{\"status\":\"error\",\"error\":\"panel deadline\",\"message\":\"panel deadline\","
       "\"roundtable\":{\"deadline_hit\":true,\"participant_failures\":[{\"seat\":1}]}}",
       503, 0, out, sizeof(out), err, sizeof(err));

   assert(rc == 1);
   assert(out[0] == '\0');
   assert(strstr(err, "aimee: panel deadline") != NULL);
   assert(strstr(err, "roundtable") == NULL);
   printf("  PASS: test_human_roundtable_failure_uses_stderr\n");
}

static void test_success_with_string_error_remains_success(void)
{
   char out[2048], err[2048];
   int rc = finish_response_capture(
       "cron.run", "{\"status\":\"ok\",\"result\":\"completed\",\"error\":\"job stderr\"}", 200, 1,
       out, sizeof(out), err, sizeof(err));

   assert(rc == 0);
   assert(err[0] == '\0');
   cJSON *printed = cJSON_Parse(out);
   assert(printed != NULL);
   assert(cJSON_GetObjectItem(printed, "status") == NULL);
   assert(strcmp(cJSON_GetObjectItem(printed, "error")->valuestring, "job stderr") == 0);
   assert(strcmp(cJSON_GetObjectItem(printed, "result")->valuestring, "completed") == 0);
   cJSON_Delete(printed);
   printf("  PASS: test_success_with_string_error_remains_success\n");
}

static void test_failed_async_run_retains_structured_result(void)
{
   cJSON *snapshot = cJSON_Parse("{\"status\":\"failed\",\"message\":\"outer failure\"}");
   cJSON *result = cJSON_Parse("{\"ok\":false,\"error\":\"panel deadline\",\"roundtable\":{"
                               "\"deadline_hit\":true,\"participant_failures\":[{\"seat\":1,"
                               "\"category\":\"deadline\"}]}}");
   assert(snapshot && result);

   cJSON *response = cli_v1_failed_run_response(result, snapshot);
   assert(response == result);
   assert(strcmp(cJSON_GetObjectItem(response, "status")->valuestring, "error") == 0);
   assert(strcmp(cJSON_GetObjectItem(response, "message")->valuestring, "panel deadline") == 0);
   cJSON *roundtable = cJSON_GetObjectItem(response, "roundtable");
   assert(cJSON_IsTrue(cJSON_GetObjectItem(roundtable, "deadline_hit")));
   assert(cJSON_GetArraySize(cJSON_GetObjectItem(roundtable, "participant_failures")) == 1);

   cJSON_Delete(response);
   cJSON_Delete(snapshot);
   printf("  PASS: test_failed_async_run_retains_structured_result\n");
}

static void test_failed_async_run_synthesizes_legacy_envelope(void)
{
   cJSON *snapshot = cJSON_Parse("{\"status\":\"cancelled\"}");
   cJSON *result = cJSON_CreateString("unstructured result");
   assert(snapshot && result);

   cJSON *response = cli_v1_failed_run_response(result, snapshot);
   cJSON *error = cJSON_GetObjectItem(response, "error");
   assert(cJSON_IsObject(error));
   assert(strcmp(cJSON_GetObjectItem(error, "message")->valuestring,
                 "run cancelled with no result") == 0);
   assert(strcmp(cJSON_GetObjectItem(error, "type")->valuestring, "run_failed") == 0);

   cJSON_Delete(response);
   cJSON_Delete(snapshot);
   printf("  PASS: test_failed_async_run_synthesizes_legacy_envelope\n");
}

static void test_remote_workspace_hidden_roots_are_rejected(void)
{
   assert(cli_ws_root_has_hidden_component("/repo/.aimee/worktrees/session/main") == 1);
   assert(cli_ws_root_has_hidden_component("/repo/.claude/worktrees/session") == 1);
   assert(cli_ws_root_has_hidden_component("/tmp/.fixture") == 1);
   assert(cli_ws_root_has_hidden_component("/repo.v1/worktrees/session") == 0);
   assert(cli_ws_root_has_hidden_component("/repo/src/.generated/file.c") == 1);
   assert(cli_ws_root_has_hidden_component("/repo/src/generated/file.c") == 0);
   printf("  PASS: test_remote_workspace_hidden_roots_are_rejected\n");
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
                   "--roundtable",
                   "deep-review",
                   "--turns",
                   "parallel",
                   "--rounds",
                   "2",
                   "--brief",
                   "focus on auth",
                   "--brief-json",
                   "{\"questions\":[\"does auth hold?\"]}",
                   "--apply"};
   cJSON *req = marshal_roundtable_review(14, argv);
   assert(req != NULL);
   assert(strcmp(cJSON_GetObjectItem(req, "method")->valuestring, "roundtable.review") == 0);
   assert(strcmp(cJSON_GetObjectItem(req, "prompt")->valuestring,
                 "review this change thoroughly") == 0);
   assert(strcmp(cJSON_GetObjectItem(req, "mode")->valuestring, "review") == 0);
   assert(strcmp(cJSON_GetObjectItem(req, "roundtable")->valuestring, "deep-review") == 0);
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
      cJSON *req = marshal_roundtable_review(3, argv);
      cJSON_Delete(req);
      _exit(0);
   }

   int status = 0;
   assert(waitpid(pid, &status, 0) == pid);
   assert(WIFEXITED(status));
   assert(WEXITSTATUS(status) == 1);
   printf("  PASS: test_delegate_roundtable_invalid_brief_json_exits\n");
}

/* Regression guard: the roundtable marshaller must fold --context-file preloads
 * into the prompt as well. It previously dropped them, so the panel received no
 * context and stalled looping for it. Mirrors
 * test_delegate_context_file_folded_into_prompt for the roundtable path. */
static void test_delegate_roundtable_context_file_folded_into_prompt(void)
{
   char path[256];
   snprintf(path, sizeof path, "%s/aimee_rt_ctx_test_XXXXXX", platform_tmpdir());
   int fd = mkstemp(path);
   assert(fd >= 0);
   const char *marker = "ROUNDTABLE_PRELOAD_MARKER_77 review_block_body";
   assert(write(fd, marker, strlen(marker)) == (ssize_t)strlen(marker));
   close(fd);

   char *argv[] = {"critique the block above", "--context-file", path};
   cJSON *req = marshal_roundtable_review(3, argv);
   assert(req != NULL);
   const cJSON *prompt = cJSON_GetObjectItem(req, "prompt");
   assert(cJSON_IsString(prompt));
   /* Original prompt text preserved... */
   assert(strstr(prompt->valuestring, "critique the block above") != NULL);
   /* ...and the file contents are injected. */
   assert(strstr(prompt->valuestring, "ROUNDTABLE_PRELOAD_MARKER_77") != NULL);
   assert(strstr(prompt->valuestring, "Source Packet: Preloaded Context") != NULL);
   assert(strstr(prompt->valuestring, path) != NULL);
   cJSON_Delete(req);

   /* Preload-only invocation (no positional prompt): the preload still becomes
    * the prompt. Exercises the base == "" branch. */
   char *argv_only[] = {"--context-file", path};
   cJSON *req_only = marshal_roundtable_review(2, argv_only);
   assert(req_only != NULL);
   const cJSON *p_only = cJSON_GetObjectItem(req_only, "prompt");
   assert(cJSON_IsString(p_only));
   /* With no positional prompt, the preload IS the prompt: no leading blank lines. */
   assert(strncmp(p_only->valuestring, "# Source Packet: Preloaded Context", 34) == 0);
   assert(strstr(p_only->valuestring, "ROUNDTABLE_PRELOAD_MARKER_77") != NULL);
   cJSON_Delete(req_only);

   /* No preload flags => prompt is unchanged (no spurious Source Packet). */
   char *argv2[] = {"a plain roundtable prompt with no preload flags"};
   cJSON *req2 = marshal_roundtable_review(1, argv2);
   assert(req2 != NULL);
   const cJSON *p2 = cJSON_GetObjectItem(req2, "prompt");
   assert(cJSON_IsString(p2));
   assert(strcmp(p2->valuestring, "a plain roundtable prompt with no preload flags") == 0);
   cJSON_Delete(req2);

   unlink(path);
   printf("  PASS: test_delegate_roundtable_context_file_folded_into_prompt\n");
}

static cJSON *marshal_roundtable_with_stdin(int argc, char **argv, const char *input)
{
   int old_stdin = dup(STDIN_FILENO);
   assert(old_stdin >= 0);
   int pipefd[2];
   assert(pipe(pipefd) == 0);
   assert(write(pipefd[1], input, strlen(input)) == (ssize_t)strlen(input));
   close(pipefd[1]);
   assert(dup2(pipefd[0], STDIN_FILENO) >= 0);
   clearerr(stdin);
   close(pipefd[0]);
   cJSON *req = marshal_roundtable_review(argc, argv);
   assert(dup2(old_stdin, STDIN_FILENO) >= 0);
   clearerr(stdin);
   close(old_stdin);
   return req;
}

static char *read_limited_with_stdin(const char *input, size_t limit)
{
   int old_stdin = dup(STDIN_FILENO);
   int pipefd[2];
   assert(old_stdin >= 0 && pipe(pipefd) == 0);
   assert(write(pipefd[1], input, strlen(input)) == (ssize_t)strlen(input));
   close(pipefd[1]);
   assert(dup2(pipefd[0], STDIN_FILENO) >= 0);
   clearerr(stdin);
   close(pipefd[0]);
   char *result = marshal_read_stdin_limited(limit);
   assert(dup2(old_stdin, STDIN_FILENO) >= 0);
   clearerr(stdin);
   close(old_stdin);
   return result;
}

static void test_roundtable_stdin_limit_is_inclusive(void)
{
   char *exact = read_limited_with_stdin("12345678", 8);
   assert(exact != NULL && strcmp(exact, "12345678") == 0);
   free(exact);
   assert(read_limited_with_stdin("123456789", 8) == NULL);
   printf("  PASS: test_roundtable_stdin_limit_is_inclusive\n");
}

static void test_roundtable_stdin_is_authoritative_artifact(void)
{
   char *argv[] = {
       "-",          "Review PR #1828 only", "--run-id", "review-pr-1828", "--artifact-stage",
       "frozen_diff"};
   cJSON *req = marshal_roundtable_with_stdin(6, argv, "diff --git a/PR1828 b/PR1828\n+marker\n");
   assert(req != NULL);
   assert(strcmp(cJSON_GetObjectItem(req, "artifact")->valuestring,
                 "diff --git a/PR1828 b/PR1828\n+marker\n") == 0);
   assert(strcmp(cJSON_GetObjectItem(req, "original_request")->valuestring,
                 "Review PR #1828 only") == 0);
   assert(strcmp(cJSON_GetObjectItem(req, "run_id")->valuestring, "review-pr-1828") == 0);
   assert(strcmp(cJSON_GetObjectItem(req, "artifact_stage")->valuestring, "frozen_diff") == 0);
   assert(cJSON_GetObjectItem(req, "prompt") == NULL);
   cJSON_Delete(req);
   printf("  PASS: test_roundtable_stdin_is_authoritative_artifact\n");
}

static void test_roundtable_path_is_read_not_forwarded(void)
{
   char path[256];
   snprintf(path, sizeof path, "%s/aimee_rt_artifact_test_XXXXXX", platform_tmpdir());
   int fd = mkstemp(path);
   assert(fd >= 0);
   const char *artifact = "diff --git a/ONLY1828 b/ONLY1828\n+exact bytes\n";
   assert(write(fd, artifact, strlen(artifact)) == (ssize_t)strlen(artifact));
   close(fd);
   char *argv[] = {path, "Implement the original request"};
   cJSON *req = marshal_roundtable_review(2, argv);
   assert(req != NULL);
   assert(strcmp(cJSON_GetObjectItem(req, "artifact")->valuestring, artifact) == 0);
   assert(strcmp(cJSON_GetObjectItem(req, "original_request")->valuestring,
                 "Implement the original request") == 0);
   assert(strstr(cJSON_GetObjectItem(req, "artifact")->valuestring, path) == NULL);
   cJSON_Delete(req);
   unlink(path);
   printf("  PASS: test_roundtable_path_is_read_not_forwarded\n");
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

   char *list_argv[] = {"--available", "--all", "--json"};
   cJSON *req = marshal_provider_list(3, list_argv);
   assert(req != NULL);
   assert(strcmp(cJSON_GetObjectItem(req, "method")->valuestring, "provider.list") == 0);
   assert(cJSON_IsTrue(cJSON_GetObjectItem(req, "available_only")));
   assert(cJSON_IsTrue(cJSON_GetObjectItem(req, "all")));
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

static void test_catalog_routes_and_marshaling(void)
{
   cli_v1_route_t route;

   char *show_lookup[] = {"show", "openrouter:anthropic/claude-opus-4.6"};
   assert(cli_v1_lookup("catalog", 2, show_lookup, &route));
   assert(strcmp(route.method, "catalog.show") == 0);
   assert(route.skip_subcmd == 1);

   char *list_lookup[] = {"list", "--capability", "vision"};
   assert(cli_v1_lookup("catalog", 3, list_lookup, &route));
   assert(strcmp(route.method, "catalog.list") == 0);
   assert(route.skip_subcmd == 1);

   cJSON *req = marshal_model_show(1, &show_lookup[1]);
   assert(req != NULL);
   assert(strcmp(cJSON_GetObjectItem(req, "method")->valuestring, "catalog.show") == 0);
   assert(strcmp(cJSON_GetObjectItem(req, "name")->valuestring,
                 "openrouter:anthropic/claude-opus-4.6") == 0);
   assert(strcmp(cJSON_GetObjectItem(req, "provider")->valuestring, "openrouter") == 0);
   assert(strcmp(cJSON_GetObjectItem(req, "model")->valuestring, "anthropic/claude-opus-4.6") == 0);
   cJSON_Delete(req);

   char *list_argv[] = {"--capability", "vision", "--json", "--open-weights"};
   req = marshal_model_list(4, list_argv);
   assert(req != NULL);
   assert(strcmp(cJSON_GetObjectItem(req, "method")->valuestring, "catalog.list") == 0);
   assert(strcmp(cJSON_GetObjectItem(req, "capability")->valuestring, "vision") == 0);
   assert(cJSON_IsTrue(cJSON_GetObjectItem(req, "json")));
   assert(cJSON_IsTrue(cJSON_GetObjectItem(req, "open_weights_only")));
   cJSON_Delete(req);

   char *show_argv[] = {"openai:gpt-4o"};
   req = marshal_model_show(1, show_argv);
   assert(req != NULL);
   assert(strcmp(cJSON_GetObjectItem(req, "method")->valuestring, "catalog.show") == 0);
   assert(strcmp(cJSON_GetObjectItem(req, "name")->valuestring, "openai:gpt-4o") == 0);
   assert(strcmp(cJSON_GetObjectItem(req, "provider")->valuestring, "openai") == 0);
   assert(strcmp(cJSON_GetObjectItem(req, "model")->valuestring, "gpt-4o") == 0);
   cJSON_Delete(req);

   printf("  PASS: test_catalog_routes_and_marshaling\n");
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

static void test_ordered_memory_commands_marshal_active_scope(void)
{
   char cwd[4096];
   assert(getcwd(cwd, sizeof(cwd)) != NULL);

   char *search_argv[] = {"needle",       "--project", "project-id", "--workspace",
                          "workspace-id", "--scope",   "all"};
   cJSON *req = marshal_memory_search(7, search_argv);
   assert(strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(req, "cwd")), cwd) == 0);
   assert(strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(req, "project")), "project-id") == 0);
   assert(strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(req, "workspace")), "workspace-id") == 0);
   assert(strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(req, "scope")), "all") == 0);
   cJSON_Delete(req);

   char *scope_argv[] = {"--scope", "all"};
   req = marshal_memory_list(2, scope_argv);
   assert(strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(req, "cwd")), cwd) == 0);
   assert(strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(req, "scope")), "all") == 0);
   cJSON_Delete(req);

   req = marshal_memory_read(2, scope_argv);
   assert(strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(req, "cwd")), cwd) == 0);
   assert(strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(req, "scope")), "all") == 0);
   cJSON_Delete(req);

   char *recall_argv[] = {"release task"};
   req = marshal_memory_recall(1, recall_argv);
   assert(strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(req, "cwd")), cwd) == 0);
   cJSON_Delete(req);

   printf("  PASS: test_ordered_memory_commands_marshal_active_scope\n");
}

static void test_memory_delete_and_supersede_routes(void)
{
   cli_v1_route_t route;

   char *delete_lookup[] = {"delete", "181"};
   assert(cli_v1_lookup("memory", 2, delete_lookup, &route));
   assert(strcmp(route.method, "memory.delete") == 0);
   assert(route.skip_subcmd == 1);

   cJSON *req = marshal_request(route.method, 1, delete_lookup + 1);
   assert(req != NULL);
   assert(strcmp(cJSON_GetObjectItem(req, "method")->valuestring, "memory.delete") == 0);
   assert((long long)cJSON_GetObjectItem(req, "id")->valuedouble == 181);
   cJSON_Delete(req);

   char *supersede_lookup[] = {"supersede", "181", "corrected fact", "--confidence=0.75",
                               "--session=release-e2e"};
   assert(cli_v1_lookup("memory", 5, supersede_lookup, &route));
   assert(strcmp(route.method, "memory.supersede") == 0);
   assert(route.skip_subcmd == 1);

   req = marshal_request(route.method, 4, supersede_lookup + 1);
   assert(req != NULL);
   assert(strcmp(cJSON_GetObjectItem(req, "method")->valuestring, "memory.supersede") == 0);
   assert((long long)cJSON_GetObjectItem(req, "old_id")->valuedouble == 181);
   assert(strcmp(cJSON_GetObjectItem(req, "new_content")->valuestring, "corrected fact") == 0);
   assert(cJSON_GetObjectItem(req, "confidence")->valuedouble == 0.75);
   assert(strcmp(cJSON_GetObjectItem(req, "session_id")->valuestring, "release-e2e") == 0);
   cJSON_Delete(req);

   printf("  PASS: test_memory_delete_and_supersede_routes\n");
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

   char *forensics_lookup[] = {"forensics"};
   assert(cli_v1_lookup("doctor", 1, forensics_lookup, &route));
   assert(strcmp(route.method, "doctor.forensics") == 0);
   const char *verb = NULL;
   assert(strcmp(cli_v1_route_for_method(route.method, &verb), "/v1/server/forensics") == 0);
   assert(strcmp(verb, "GET") == 0);

   printf("  PASS: test_server_status_route_lookup\n");
}

/* Asking for a review and not getting one must not look like approval.
 *
 * Measured against a real aimee-server with the roundtable module attached: a
 * review with no saved roundtable came back status=pending,
 * pause_reason=panel_unreachable, artifact="" -- and `aimee roundtable review`
 * printed zero bytes and exited 0. A pre-merge hook, CI job or agent gating on
 * that exit code would have read "no review happened" as "the panel approved
 * it", which is the one conclusion it must never draw. */
static void test_roundtable_review_without_an_artifact_is_a_failure(void)
{
   cJSON *resp = cJSON_CreateObject();
   cJSON_AddStringToObject(resp, "status", "pending");
   cJSON_AddStringToObject(resp, "pause_reason", "panel_unreachable");
   cJSON_AddStringToObject(resp, "artifact", "");
   assert(roundtable_review_response_is_failure(resp) == 1);

   /* An actual review is a success, whatever else the envelope carries. */
   cJSON_ReplaceItemInObject(resp, "artifact", cJSON_CreateString("## Findings\n- one"));
   assert(roundtable_review_response_is_failure(resp) == 0);

   /* A missing artifact field is the same as an empty one: no review. */
   cJSON_DeleteItemFromObject(resp, "artifact");
   assert(roundtable_review_response_is_failure(resp) == 1);
   cJSON_Delete(resp);

   /* No response at all cannot be an approval either. */
   assert(roundtable_review_response_is_failure(NULL) == 1);
   printf("  PASS: a review without an artifact fails\n");
}

static void test_agent_probe_failure_controls_exit_status(void)
{
   cJSON *resp = cJSON_CreateObject();
   cJSON_AddBoolToObject(resp, "model_available", 0);
   cJSON_AddBoolToObject(resp, "execution_ok", 0);
   assert(agent_probe_response_is_failure(resp) == 1);
   cJSON_ReplaceItemInObject(resp, "execution_ok", cJSON_CreateTrue());
   /* A successful execution wins over a hosted provider's unavailable /models. */
   assert(agent_probe_response_is_failure(resp) == 0);
   cJSON_DeleteItemFromObject(resp, "execution_ok");
   assert(agent_probe_response_is_failure(resp) == 1); /* --no-run model probe */
   cJSON_ReplaceItemInObject(resp, "model_available", cJSON_CreateTrue());
   assert(agent_probe_response_is_failure(resp) == 0);
   cJSON_Delete(resp);

   resp = cJSON_CreateObject();
   assert(agent_probe_response_is_failure(resp) == 0); /* old server compatibility */
   cJSON_Delete(resp);
   printf("  PASS: test_agent_probe_failure_controls_exit_status\n");
}

static void test_kb_docs_push_route_and_marshal(void)
{
   cli_v1_route_t route;
   char path1[256];
   snprintf(path1, sizeof path1, "%s/aimee-cli-doc-one-XXXXXX", platform_tmpdir());
   char path2[256];
   snprintf(path2, sizeof path2, "%s/aimee-cli-doc-two-XXXXXX", platform_tmpdir());
   int fd1 = mkstemp(path1), fd2 = mkstemp(path2);
   assert(fd1 >= 0 && fd2 >= 0);
   assert(write(fd1, "alpha doc\n", 10) == 10);
   assert(write(fd2, "beta doc\n", 9) == 9);
   close(fd1);
   close(fd2);

   char *lookup[] = {"docs", "push", "--scope", "project", path1, path2};
   assert(cli_v1_lookup("kb", 6, lookup, &route));
   assert(strcmp(route.method, "kb.docs.push") == 0);
   assert(route.skip_subcmd == 2);

   char *args[] = {"--scope", "project", path1, path2};
   cJSON *req = marshal_request(route.method, 4, args);
   assert(req != NULL);
   assert(strcmp(cJSON_GetObjectItem(req, "method")->valuestring, "kb.docs.push") == 0);
   assert(strcmp(cJSON_GetObjectItem(req, "scope")->valuestring, "project") == 0);
   cJSON *paths = cJSON_GetObjectItem(req, "paths");
   assert(cJSON_IsArray(paths) && cJSON_GetArraySize(paths) == 2);
   assert(strcmp(cJSON_GetArrayItem(paths, 0)->valuestring, path1) == 0);
   assert(strcmp(cJSON_GetArrayItem(paths, 1)->valuestring, path2) == 0);
   cJSON *documents = cJSON_GetObjectItem(req, "documents");
   assert(cJSON_IsArray(documents) && cJSON_GetArraySize(documents) == 2);
   cJSON *doc1 = cJSON_GetArrayItem(documents, 0);
   assert(strcmp(cJSON_GetObjectItem(doc1, "path")->valuestring, path1) == 0);
   assert(strcmp(cJSON_GetObjectItem(doc1, "content")->valuestring, "alpha doc\n") == 0);
   cJSON_Delete(req);

   assert(marshal_kb_docs_push(0, NULL) == NULL);
   unlink(path1);
   unlink(path2);

   printf("  PASS: test_kb_docs_push_route_and_marshal\n");
}

static void test_kb_remote_status_routes(void)
{
   cli_v1_route_t route;
   const char *verb = NULL;

   char *health_lookup[] = {"health"};
   assert(cli_v1_lookup("kb", 1, health_lookup, &route));
   assert(strcmp(route.method, "kb.health") == 0);
   assert(strcmp(cli_v1_route_for_method(route.method, &verb), "/v1/kb/status") == 0);
   assert(strcmp(verb, "GET") == 0);

   char *ingest_lookup[] = {"ingest", "status"};
   assert(cli_v1_lookup("kb", 2, ingest_lookup, &route));
   assert(strcmp(route.method, "kb.ingest.status") == 0);
   assert(strcmp(cli_v1_route_for_method(route.method, &verb), "/v1/kb/ingest/status") == 0);
   assert(strcmp(verb, "GET") == 0);

   char *status_lookup[] = {"status", "release-e2e"};
   assert(cli_v1_lookup("kb", 2, status_lookup, &route));
   cJSON *req = marshal_request(route.method, 1, status_lookup + 1);
   assert(req != NULL);
   assert(strcmp(cJSON_GetObjectItem(req, "project")->valuestring, "release-e2e") == 0);
   cJSON_Delete(req);

   printf("  PASS: test_kb_remote_status_routes\n");
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

/* An explicit path= must actually reach the server.
 *
 * cJSON permits duplicate keys and cJSON_GetObjectItemCaseSensitive returns the
 * FIRST match. path=<cwd> was added before the caller's arguments were parsed, so
 * `aimee git verify path=<repo>` produced two "path" entries and the server read
 * the cwd every time. Measured: verify resolved /var/lib (the shell's directory)
 * while explicitly told path=/repo. The error message's own advice to pass path
 * was therefore impossible to act on.
 *
 * Assert the value the server will actually read, and that exactly one exists. */
static int count_keys(cJSON *obj, const char *key)
{
   int n = 0;
   for (cJSON *it = obj ? obj->child : NULL; it; it = it->next)
      if (it->string && strcmp(it->string, key) == 0)
         n++;
   return n;
}

static void test_git_verify_explicit_path_wins_over_cwd(void)
{
   char path_arg[] = "path=/tmp/some-other-repo";
   char *argv[] = {path_arg};
   cJSON *req = marshal_git_verify(1, argv);
   assert(req != NULL);
   cJSON *args = cJSON_GetObjectItem(req, "arguments");
   assert(cJSON_IsObject(args));

   assert(count_keys(args, "path") == 1);
   cJSON *p = cJSON_GetObjectItemCaseSensitive(args, "path");
   assert(cJSON_IsString(p));
   assert(strcmp(p->valuestring, "/tmp/some-other-repo") == 0);
   cJSON_Delete(req);

   /* With no path given, the cwd is still the default. */
   char async_arg2[] = "--async=false";
   char *argv2[] = {async_arg2};
   req = marshal_git_verify(1, argv2);
   assert(req != NULL);
   args = cJSON_GetObjectItem(req, "arguments");
   assert(count_keys(args, "path") == 1);
   p = cJSON_GetObjectItemCaseSensitive(args, "path");
   assert(cJSON_IsString(p) && p->valuestring[0] == '/');
   cJSON_Delete(req);

   printf("  PASS: test_git_verify_explicit_path_wins_over_cwd\n");
}

static void test_get_help_route_marshaled(void)
{
   cli_v1_route_t route;
   char *topic_lookup[] = {"work", "queue"};
   assert(cli_v1_lookup("get_help", 2, topic_lookup, &route));
   assert(strcmp(route.method, "get_help") == 0);
   assert(strcmp(route.server_method, "help.get") == 0);
   assert(route.skip_subcmd == 0);

   cJSON *req = marshal_request(route.method, 2, topic_lookup);
   assert(req != NULL);
   assert(strcmp(cJSON_GetObjectItem(req, "method")->valuestring, "help.get") == 0);
   assert(strcmp(cJSON_GetObjectItem(req, "tool")->valuestring, "get_help") == 0);
   cJSON *args = cJSON_GetObjectItem(req, "arguments");
   assert(cJSON_IsObject(args));
   assert(strcmp(cJSON_GetObjectItem(args, "topic")->valuestring, "work queue") == 0);
   cJSON_Delete(req);

   assert(cli_v1_lookup("get-help", 2, topic_lookup, &route));
   assert(strcmp(route.method, "get_help") == 0);
   assert(strcmp(route.server_method, "help.get") == 0);

   req = marshal_request(route.method, 0, NULL);
   assert(req != NULL);
   assert(strcmp(cJSON_GetObjectItem(req, "method")->valuestring, "help.get") == 0);
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

/* Regression: `aimee index callers <symbol> --json` must not treat the output-mode
 * `--json` flag as the OPTIONAL `project` positional. Before the fix the marshaler
 * read raw argv[1], sending project="--json" — which matches no project, so the
 * server returned an empty caller set for every symbol whenever --json was used
 * without an explicit project. */
static void test_index_find_callers_json_flag_not_project(void)
{
   char *argv[] = {"helper_double", "--json"};
   cJSON *req = marshal_index_find_callers(2, argv);
   assert(req != NULL);
   assert(strcmp(cJSON_GetObjectItem(req, "method")->valuestring, "index.find_callers") == 0);
   assert(strcmp(cJSON_GetObjectItem(req, "symbol")->valuestring, "helper_double") == 0);
   /* No project was given, so the key must be absent (never "--json"). */
   assert(cJSON_GetObjectItem(req, "project") == NULL);
   cJSON_Delete(req);

   /* An explicit project is still forwarded, with --json (trailing) ignored. */
   char *argv2[] = {"helper_double", "myproj", "--json"};
   cJSON *req2 = marshal_index_find_callers(3, argv2);
   assert(req2 != NULL);
   assert(strcmp(cJSON_GetObjectItem(req2, "symbol")->valuestring, "helper_double") == 0);
   assert(strcmp(cJSON_GetObjectItem(req2, "project")->valuestring, "myproj") == 0);
   cJSON_Delete(req2);

   /* Scope survives proxy marshalling and is never consumed as the optional
    * project positional.  cwd lets the server resolve scope=current. */
   char *argv3[] = {"helper_double", "--scope", "all", "--json"};
   cJSON *req3 = marshal_index_find_callers(4, argv3);
   assert(req3 != NULL);
   assert(strcmp(cJSON_GetObjectItem(req3, "scope")->valuestring, "all") == 0);
   assert(cJSON_GetObjectItem(req3, "project") == NULL);
   assert(cJSON_IsString(cJSON_GetObjectItem(req3, "cwd")));
   cJSON_Delete(req3);

   printf("  PASS: test_index_find_callers_json_flag_not_project\n");
}

static void test_index_current_project_proxy_context(void)
{
   char *find_argv[] = {"workspace_repo_identity", "--scope", "current", "--json"};
   cJSON *find = marshal_index_find(4, find_argv);
   assert(find != NULL);
   assert(strcmp(cJSON_GetObjectItem(find, "identifier")->valuestring, "workspace_repo_identity") ==
          0);
   assert(strcmp(cJSON_GetObjectItem(find, "scope")->valuestring, "current") == 0);
   assert(cJSON_IsString(cJSON_GetObjectItem(find, "cwd")));
   cJSON_Delete(find);

   char *file_argv[] = {"src/index.c", "--json"};
   cJSON *structure = marshal_index_structure(2, file_argv);
   assert(structure != NULL);
   assert(strcmp(cJSON_GetObjectItem(structure, "file_path")->valuestring, "src/index.c") == 0);
   assert(cJSON_GetObjectItem(structure, "project") == NULL);
   assert(cJSON_IsString(cJSON_GetObjectItem(structure, "cwd")));
   cJSON_Delete(structure);

   char *legacy_argv[] = {"legacy-project", "src/index.c"};
   cJSON *blast = marshal_index_blast_radius(2, legacy_argv);
   assert(blast != NULL);
   assert(strcmp(cJSON_GetObjectItem(blast, "project")->valuestring, "legacy-project") == 0);
   assert(strcmp(cJSON_GetObjectItem(blast, "file_path")->valuestring, "src/index.c") == 0);
   cJSON_Delete(blast);

   printf("  PASS: test_index_current_project_proxy_context\n");
}

static void test_kb_search_proxy_context(void)
{
   char *current_argv[] = {"stable identity", "--scope", "current"};
   cJSON *current = marshal_kb_search(3, current_argv);
   assert(current != NULL);
   assert(strcmp(cJSON_GetObjectItem(current, "method")->valuestring, "kb.search") == 0);
   assert(strcmp(cJSON_GetObjectItem(current, "scope")->valuestring, "current") == 0);
   assert(cJSON_IsString(cJSON_GetObjectItem(current, "cwd")));
   assert(cJSON_GetObjectItem(current, "project") == NULL);
   cJSON_Delete(current);

   char *all_argv[] = {"stable identity", "--scope", "all"};
   cJSON *all = marshal_kb_search(3, all_argv);
   assert(all != NULL);
   assert(strcmp(cJSON_GetObjectItem(all, "scope")->valuestring, "all") == 0);
   assert(cJSON_GetObjectItem(all, "project") == NULL);
   cJSON_Delete(all);

   printf("  PASS: test_kb_search_proxy_context\n");
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

   char path[256];
   snprintf(path, sizeof path, "%s/aimee-insights-output-XXXXXX", platform_tmpdir());
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

   char home[256];
   snprintf(home, sizeof home, "%s/aimee-rpce-XXXXXX", platform_tmpdir());
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
   char path[256];
   snprintf(path, sizeof path, "%s/aimee_ctx_test_XXXXXX", platform_tmpdir());
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

/* --verify and --scope govern SPEND (escalation) and what a seat may attempt
 * (scope), and are honoured only by the in-process delegate path. The marshaller
 * forwards a fixed allowlist and drops the rest, so routing a run with either
 * flag used to return a normal, successful-looking result while the verifier
 * never ran. Refusing the run is the only honest answer until they are plumbed
 * end to end - so pin that it EXITS rather than quietly marshaling. */
static int marshal_delegate_exit_status(char **argv, int argc)
{
   pid_t pid = fork();
   assert(pid >= 0);
   if (pid == 0)
   {
      /* The guard writes to stderr before exiting; keep the test output clean. */
      freopen("/dev/null", "w", stderr);
      cJSON *req = marshal_delegate(argc, argv);
      /* Reached only if the guard did NOT fire. */
      cJSON_Delete(req);
      _exit(0);
   }
   int status = 0;
   assert(waitpid(pid, &status, 0) == pid);
   assert(WIFEXITED(status));
   return WEXITSTATUS(status);
}

static void test_delegate_unsupported_routed_flags_are_refused(void)
{
   /* --verify runs a caller-supplied shell command and its exit status alone
    * decides whether a model was inadequate, so it is neither forwarded (that
    * would put caller-supplied code on a shared server and hand the caller
    * control of spend) nor runnable client-side (the thin client links no
    * delegate engine). Refuse rather than ignore. */
   char *verify_argv[] = {"review", "task", "--verify", "false"};
   assert(marshal_delegate_exit_status(verify_argv, 4) == 1);

   /* A bad --scope spelling fails fast at the client instead of reaching a
    * server that would parse it as UNSET and admit every seat. */
   char *bad_scope_argv[] = {"review", "task", "--scope", "enormous"};
   assert(marshal_delegate_exit_status(bad_scope_argv, 4) == 1);

   /* A run WITHOUT them still marshals normally - the guard must not be a
    * blanket refusal. */
   char *plain_argv[] = {"review", "task"};
   assert(marshal_delegate_exit_status(plain_argv, 2) == 0);
}

/* --scope is pure routing policy carrying no caller-supplied code, and the
 * server is already choosing the seat - so unlike --verify it IS forwarded, and
 * the ceiling is enforced server-side. It used to be dropped by the allowlist,
 * which left max_scope never binding for any routed run. */
static void test_delegate_scope_is_forwarded(void)
{
   char *argv[] = {"review", "task", "--scope", "bounded"};
   cJSON *req = marshal_delegate(4, argv);
   cJSON *scope = cJSON_GetObjectItemCaseSensitive(req, "scope");
   assert(cJSON_IsString(scope));
   assert(strcmp(scope->valuestring, "bounded") == 0);
   cJSON_Delete(req);

   char *whole[] = {"review", "task", "--scope", "whole_task"};
   req = marshal_delegate(4, whole);
   scope = cJSON_GetObjectItemCaseSensitive(req, "scope");
   assert(cJSON_IsString(scope) && strcmp(scope->valuestring, "whole_task") == 0);
   cJSON_Delete(req);

   /* Absent unless asked for: an omitted flag must not imply a ceiling. */
   char *plain[] = {"review", "task"};
   req = marshal_delegate(2, plain);
   assert(cJSON_GetObjectItemCaseSensitive(req, "scope") == NULL);
   cJSON_Delete(req);
}

/* --- kb grant text rendering. These four commands printed NOTHING in text mode
 * until the printers were added: every outcome field was --json-only. Two of the
 * fields are safety-relevant (is_member=false means the grant is inert; a revoke
 * leaves an issued token alive until it expires), so these assert on the operator's
 * actual bytes, split by stream, rather than on the printer returning. --- */
static void grant_render(const char *method, const char *json, char *out, size_t out_cap, char *err,
                         size_t err_cap)
{
   cJSON *resp = cJSON_Parse(json);
   assert(resp != NULL);

   char opath[256];
   snprintf(opath, sizeof opath, "%s/aimee-grant-out-XXXXXX", platform_tmpdir());
   char epath[256];
   snprintf(epath, sizeof epath, "%s/aimee-grant-err-XXXXXX", platform_tmpdir());
   int ofd = mkstemp(opath), efd = mkstemp(epath);
   assert(ofd >= 0 && efd >= 0);
   int old_out = dup(STDOUT_FILENO), old_err = dup(STDERR_FILENO);
   assert(old_out >= 0 && old_err >= 0);
   fflush(stdout);
   fflush(stderr);
   assert(dup2(ofd, STDOUT_FILENO) >= 0);
   assert(dup2(efd, STDERR_FILENO) >= 0);

   print_text_output(method, resp);

   fflush(stdout);
   fflush(stderr);
   assert(dup2(old_out, STDOUT_FILENO) >= 0);
   assert(dup2(old_err, STDERR_FILENO) >= 0);
   close(old_out);
   close(old_err);
   close(ofd);
   close(efd);

   FILE *f = fopen(opath, "rb");
   assert(f != NULL);
   size_t n = fread(out, 1, out_cap - 1, f);
   fclose(f);
   out[n] = '\0';
   f = fopen(epath, "rb");
   assert(f != NULL);
   n = fread(err, 1, err_cap - 1, f);
   fclose(f);
   err[n] = '\0';
   unlink(opath);
   unlink(epath);
   cJSON_Delete(resp);
}

static void test_grant_set_created_vs_changed_from_off(void)
{
   char out[2048], err[2048];

   /* previous_tier ABSENT: the grant did not exist. */
   grant_render("kb.grant.set", "{\"changed\":true,\"was_revoked\":false,\"is_member\":true}", out,
                sizeof(out), err, sizeof(err));
   assert(strstr(out, "created") != NULL);
   assert(strstr(out, "changed from") == NULL);
   assert(err[0] == '\0'); /* a member grant that took effect warns about nothing */

   /* previous_tier "off": it DID exist and was off. Must not render like "created". */
   grant_render(
       "kb.grant.set",
       "{\"changed\":true,\"was_revoked\":false,\"is_member\":true,\"previous_tier\":\"off\"}", out,
       sizeof(out), err, sizeof(err));
   assert(strstr(out, "changed from off") != NULL);
   assert(strstr(out, "created") == NULL);
}

static void test_grant_set_unchanged_reports_current_tier(void)
{
   char out[2048], err[2048];
   grant_render(
       "kb.grant.set",
       "{\"changed\":false,\"was_revoked\":false,\"is_member\":true,\"previous_tier\":\"data\"}",
       out, sizeof(out), err, sizeof(err));
   assert(strstr(out, "unchanged") != NULL);
   assert(strstr(out, "data") != NULL);
}

static void test_grant_set_warns_when_subject_is_not_a_member(void)
{
   char out[2048], err[2048];
   grant_render("kb.grant.set", "{\"changed\":true,\"was_revoked\":false,\"is_member\":false}", out,
                sizeof(out), err, sizeof(err));
   /* The grant was written, so the outcome is still success on stdout... */
   assert(strstr(out, "created") != NULL);
   /* ...but the operator MUST be told it does nothing yet. */
   assert(strstr(err, "not a member") != NULL);
   assert(strstr(err, "no effect") != NULL);
}

static void test_grant_set_reports_reinstated_revocation(void)
{
   char out[2048], err[2048];
   grant_render("kb.grant.set", "{\"changed\":true,\"was_revoked\":true,\"is_member\":true}", out,
                sizeof(out), err, sizeof(err));
   assert(strstr(out, "reinstated") != NULL);
}

static void test_grant_set_missing_outcome_is_not_reported_as_success(void)
{
   char out[2048], err[2048];
   /* A response without `changed` is a protocol fault. Rendering "unchanged" for it
    * would be a confident false claim about authorization state. */
   grant_render("kb.grant.set", "{\"is_member\":true}", out, sizeof(out), err, sizeof(err));
   assert(strstr(out, "unchanged") == NULL);
   assert(strstr(out, "created") == NULL);
   assert(strstr(err, "no outcome") != NULL);
}

static void test_grant_revoke_found_warns_about_residual_token(void)
{
   char out[2048], err[2048];
   grant_render("kb.grant.revoke", "{\"found\":true}", out, sizeof(out), err, sizeof(err));
   assert(strstr(out, "revoked") != NULL);
   /* Access is gone SHORTLY, not immediately; the operator cannot infer this. */
   assert(strstr(err, "300s") != NULL);
}

static void test_grant_revoke_not_found_does_not_look_like_success(void)
{
   char out[2048], err[2048];
   grant_render("kb.grant.revoke", "{\"found\":false}", out, sizeof(out), err, sizeof(err));
   assert(strstr(out, "no grant existed") != NULL);
   /* No revocation happened, so the residual-token note must NOT appear. */
   assert(strstr(err, "300s") == NULL);
}

static void test_grant_revoke_missing_found_is_not_reported_as_absent(void)
{
   char out[2048], err[2048];
   grant_render("kb.grant.revoke", "{}", out, sizeof(out), err, sizeof(err));
   assert(strstr(out, "no grant existed") == NULL);
   assert(strstr(err, "no outcome") != NULL);
}

static void test_grant_list_renders_rows_and_marks_revoked(void)
{
   char out[4096], err[2048];
   grant_render("kb.grant.list",
                "{\"grants\":["
                "{\"subject\":\"oidc:iss:alice\",\"tier\":\"data\",\"granted_by\":\"owner\","
                "\"created_at\":\"2026-07-01T00:00:00Z\",\"updated_at\":\"2026-07-01T00:00:00Z\"},"
                "{\"subject\":\"oidc:iss:bob\",\"tier\":\"full\",\"granted_by\":\"owner\","
                "\"created_at\":\"2026-07-02T00:00:00Z\",\"updated_at\":\"2026-07-03T00:00:00Z\","
                "\"revoked_at\":\"2026-07-04T00:00:00Z\"}],\"truncated\":false}",
                out, sizeof(out), err, sizeof(err));
   assert(strstr(out, "oidc:iss:alice") != NULL);
   assert(strstr(out, "data") != NULL);
   assert(strstr(out, "active since") != NULL);
   /* A revoked grant must be visibly distinct from a live one. */
   assert(strstr(out, "revoked 2026-07-04T00:00:00Z") != NULL);
   assert(err[0] == '\0');
}

static void test_grant_list_empty_says_so(void)
{
   char out[2048], err[2048];
   /* Printing nothing is what the missing formatter did, and is indistinguishable
    * from a broken command. */
   grant_render("kb.grant.list", "{\"grants\":[],\"truncated\":false}", out, sizeof(out), err,
                sizeof(err));
   assert(strstr(out, "no write-tier grants") != NULL);
}

static void test_grant_list_truncation_is_surfaced(void)
{
   char out[2048], err[2048];
   grant_render("kb.grant.list",
                "{\"grants\":[{\"subject\":\"s\",\"tier\":\"data\",\"granted_by\":\"owner\","
                "\"created_at\":\"t\",\"updated_at\":\"t\"}],\"truncated\":true}",
                out, sizeof(out), err, sizeof(err));
   /* Otherwise a capped page reads as a complete answer. */
   assert(strstr(err, "more grants exist") != NULL);
}

static void test_grant_show_shares_the_list_renderer(void)
{
   char out[2048], err[2048];
   grant_render("kb.grant.show",
                "{\"grants\":[{\"subject\":\"oidc:iss:alice\",\"tier\":\"full\","
                "\"granted_by\":\"owner\",\"created_at\":\"t\",\"updated_at\":\"t\"}],"
                "\"truncated\":false}",
                out, sizeof(out), err, sizeof(err));
   assert(strstr(out, "oidc:iss:alice") != NULL);
   assert(strstr(out, "full") != NULL);
}

/* The CLI refuses an oversized /v1 body itself, because the listener drops one
 * before parsing and the client could otherwise only report it as "could not
 * reach the endpoint" -- blaming a server that is up and answering. That refusal
 * is only correct while the mirrored cap equals the server's real one. */
static void test_cli_v1_body_cap_matches_server(void)
{
   assert(CLI_V1_MAX_BODY == SHTTP_MAX_BODY);
   assert(CLI_V1_MAX_ROUNDTABLE_BODY == SHTTP_MAX_ROUNDTABLE_BODY);
   printf("  cli_v1_body_cap_matches_server: ok (%d / %d)\n", (int)CLI_V1_MAX_BODY,
          (int)CLI_V1_MAX_ROUNDTABLE_BODY);
}

int main(void)
{
   test_cli_v1_body_cap_matches_server();
   printf("test_cli_v1_delegate\n");
   test_remote_workspace_hidden_roots_are_rejected();
   test_json_error_envelopes_remain_structured();
   test_json_roundtable_failure_preserves_user_visible_envelope();
   test_human_roundtable_failure_uses_stderr();
   test_success_with_string_error_remains_success();
   test_failed_async_run_retains_structured_result();
   test_failed_async_run_synthesizes_legacy_envelope();
   test_delegate_context_file_folded_into_prompt();
   test_delegate_max_turns_marshaled();
   test_delegate_tools_named_toolset_marshaled();
   test_delegate_zero_max_turns_marshaled();
   test_delegate_provider_model_marshaled();
   test_delegate_persona_marshaled();
   test_delegate_roundtable_brief_marshaled();
   test_delegate_roundtable_invalid_brief_json_exits();
   test_delegate_roundtable_context_file_folded_into_prompt();
   test_roundtable_stdin_is_authoritative_artifact();
   test_roundtable_path_is_read_not_forwarded();
   test_roundtable_stdin_limit_is_inclusive();
   test_delegate_prompt_stdin_marshaled();
   test_delegate_status_multiple_ids_marshaled();
   test_delegate_log_rejects_ignored_args();
   test_delegate_status_result_options_marshaled();
   test_delegate_prompt_stdin_rejects_prompt_file();
   test_delegate_depth_requires_parent_env();
   test_provider_routes_and_marshaling();
   test_catalog_routes_and_marshaling();
   test_memory_show_alias_route();
   test_memory_stats_route();
   test_ordered_memory_commands_marshal_active_scope();
   test_memory_delete_and_supersede_routes();
   test_server_status_route_lookup();
   test_agent_probe_failure_controls_exit_status();
   test_roundtable_review_without_an_artifact_is_a_failure();
   test_kb_docs_push_route_and_marshal();
   test_kb_remote_status_routes();
   test_git_verify_failure_detection();
   test_git_verify_marshaled_with_session_id();
   test_git_verify_explicit_path_wins_over_cwd();
   test_get_help_route_marshaled();
   test_subcommand_json_flag_is_output_mode();
   test_grant_set_created_vs_changed_from_off();
   test_grant_set_unchanged_reports_current_tier();
   test_grant_set_warns_when_subject_is_not_a_member();
   test_grant_set_reports_reinstated_revocation();
   test_grant_set_missing_outcome_is_not_reported_as_success();
   test_grant_revoke_found_warns_about_residual_token();
   test_grant_revoke_not_found_does_not_look_like_success();
   test_grant_revoke_missing_found_is_not_reported_as_absent();
   test_grant_list_renders_rows_and_marks_revoked();
   test_grant_list_empty_says_so();
   test_grant_list_truncation_is_surfaced();
   test_grant_show_shares_the_list_renderer();
   test_index_find_callers_json_flag_not_project();
   test_index_current_project_proxy_context();
   test_kb_search_proxy_context();
   test_trigger_routes_lookup();
   test_dogfood_routes_and_marshaling();
   test_eval_routes_and_marshaling();
   test_trigger_fire_token_marshaled();
   test_cron_routes_and_marshaling();
   test_session_brief_route_marshaled();
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
   test_delegate_unsupported_routed_flags_are_refused();
   test_delegate_scope_is_forwarded();
   printf("All tests passed.\n");
   return 0;
}
