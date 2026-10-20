/* Codex JSON-RPC parsing and child protocol boundary fixtures. */
#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "cli_codex.h"
#include "provider_cli_adapter.h"
#include "server_session_control.h"
#include <aimee/ir/session_event.h>
#include "platform_test_util.h" /* platform_tmpdir: honour TMPDIR, do not leak into /tmp */

/* Minimal harnesses have no bound server turn to cancel. */
int agent_request_cancelled(void)
{
   return 0;
}

static void test_execute_rejects_missing_inputs(void)
{
   agent_result_t out;
   agent_t agent = {0};
   assert(agent_execute_cli_codex_at_cwd(&agent, NULL, NULL, "prompt", NULL) == -1);
   assert(agent_execute_cli_codex_at_cwd(NULL, NULL, NULL, "prompt", &out) == -1);
   assert(strcmp(out.error, "missing agent") == 0);
   assert(codex_provider_cli_adapter.execute(NULL, &out) == -1);
   provider_cli_cfg_t cfg = {.agent = &agent, .user_prompt = "prompt"};
   assert(codex_provider_cli_adapter.execute(&cfg, NULL) == -1);
}

static void test_parse_agent_message_extracts_text(void)
{
   const char *line = "{\"jsonrpc\":\"2.0\",\"method\":\"item/completed\",\"params\":{"
                      "\"item\":{\"type\":\"agentMessage\",\"text\":\"hello world\"}}}";
   char *out = NULL;
   assert(cli_codex_parse_agent_message(line, &out) == 1);
   assert(out != NULL);
   assert(strcmp(out, "hello world") == 0);
   free(out);
}

static void test_parse_agent_message_skips_other_item_types(void)
{
   const char *line = "{\"jsonrpc\":\"2.0\",\"method\":\"item/completed\",\"params\":{"
                      "\"item\":{\"type\":\"toolCall\",\"text\":\"unused\"}}}";
   char *out = NULL;
   assert(cli_codex_parse_agent_message(line, &out) == 0);
   assert(out == NULL);
}

static void test_parse_agent_message_skips_other_methods(void)
{
   const char *line = "{\"jsonrpc\":\"2.0\",\"method\":\"turn/started\",\"params\":{}}";
   char *out = NULL;
   assert(cli_codex_parse_agent_message(line, &out) == 0);
   assert(out == NULL);
}

static void test_parse_agent_message_handles_invalid_json(void)
{
   char *out = NULL;
   assert(cli_codex_parse_agent_message("not json {", &out) == 0);
   assert(out == NULL);
   assert(cli_codex_parse_agent_message("", &out) == 0);
   assert(cli_codex_parse_agent_message(NULL, &out) == 0);
}

static void test_parse_agent_message_empty_text_is_skipped(void)
{
   const char *line = "{\"method\":\"item/completed\",\"params\":{"
                      "\"item\":{\"type\":\"agentMessage\",\"text\":\"\"}}}";
   char *out = NULL;
   assert(cli_codex_parse_agent_message(line, &out) == 0);
   assert(out == NULL);
}

static void test_parse_token_usage_extracts_input_output(void)
{
   const char *line = "{\"method\":\"thread/tokenUsage/updated\",\"params\":{"
                      "\"tokenUsage\":{\"last\":{\"inputTokens\":1234,\"outputTokens\":56}}}}";
   int p = -1, c = -1;
   assert(cli_codex_parse_token_usage(line, &p, &c) == 1);
   assert(p == 1234);
   assert(c == 56);
}

static void test_parse_token_usage_missing_fields_default_zero(void)
{
   const char *line = "{\"method\":\"thread/tokenUsage/updated\",\"params\":{"
                      "\"tokenUsage\":{\"last\":{\"inputTokens\":42}}}}";
   int p = -1, c = -1;
   assert(cli_codex_parse_token_usage(line, &p, &c) == 1);
   assert(p == 42);
   assert(c == 0);
}

static void test_parse_token_usage_skips_other_methods(void)
{
   const char *line = "{\"method\":\"item/completed\",\"params\":{}}";
   int p = -1, c = -1;
   assert(cli_codex_parse_token_usage(line, &p, &c) == 0);
   /* On miss, helpers leave caller's vars alone — we use sentinel -1 to
    * verify that. */
   assert(p == -1);
   assert(c == -1);
}

static void test_parse_token_usage_null_outparams_ok(void)
{
   const char *line = "{\"method\":\"thread/tokenUsage/updated\",\"params\":{"
                      "\"tokenUsage\":{\"last\":{\"inputTokens\":7,\"outputTokens\":8}}}}";
   assert(cli_codex_parse_token_usage(line, NULL, NULL) == 1);
}

static void test_parse_error_notification_unwraps_nested_message(void)
{
   const char *line =
       "{\"method\":\"error\",\"params\":{\"error\":{\"message\":\"{\\\"type\\\":\\\"error\\\","
       "\\\"status\\\":400,\\\"error\\\":{\\\"type\\\":\\\"invalid_request_error\\\","
       "\\\"message\\\":\\\"The model is not supported.\\\"}}\"}}}";
   char *out = NULL;
   assert(cli_codex_parse_error_message(line, &out) == 1);
   assert(out != NULL);
   assert(strcmp(out, "The model is not supported.") == 0);
   free(out);
}

static void test_parse_turn_completed_error(void)
{
   const char *line = "{\"method\":\"turn/completed\",\"params\":{\"turn\":{\"status\":\"failed\","
                      "\"error\":{\"message\":\"codex failed\"}}}}";
   char *out = NULL;
   assert(cli_codex_parse_error_message(line, &out) == 1);
   assert(out != NULL);
   assert(strcmp(out, "codex failed") == 0);
   free(out);
}

static void test_parse_error_message_skips_success(void)
{
   char *out = NULL;
   assert(cli_codex_parse_error_message("{\"method\":\"turn/completed\",\"params\":{}}", &out) ==
          0);
   assert(out == NULL);
   assert(cli_codex_parse_error_message("not json", &out) == 0);
   assert(out == NULL);
}

static void test_parse_method_extracts_string(void)
{
   char buf[64];
   assert(cli_codex_parse_method("{\"method\":\"turn/completed\",\"params\":{}}", buf,
                                 sizeof(buf)) == 1);
   assert(strcmp(buf, "turn/completed") == 0);
}

static void test_parse_method_response_envelope_has_no_method(void)
{
   /* JSON-RPC responses carry "id" + "result" but no "method". */
   char buf[64] = "untouched";
   assert(cli_codex_parse_method("{\"id\":1,\"result\":{}}", buf, sizeof(buf)) == 0);
   /* On miss, the helper does not write to out (sentinel preserved). */
   assert(strcmp(buf, "untouched") == 0);
}

static void test_parse_method_truncates_long_names(void)
{
   char buf[8];
   assert(cli_codex_parse_method("{\"method\":\"abcdefghij\"}", buf, sizeof(buf)) == 1);
   /* snprintf truncates with NUL terminator within outsz. */
   assert(strlen(buf) == 7);
   assert(strcmp(buf, "abcdefg") == 0);
}

static void test_parse_method_invalid_inputs(void)
{
   char buf[64];
   assert(cli_codex_parse_method(NULL, buf, sizeof(buf)) == 0);
   assert(cli_codex_parse_method("{\"method\":\"x\"}", NULL, sizeof(buf)) == 0);
   assert(cli_codex_parse_method("{\"method\":\"x\"}", buf, 0) == 0);
   assert(cli_codex_parse_method("not json", buf, sizeof(buf)) == 0);
}

static void test_parse_server_request_method_extracts_approval(void)
{
   const char *line = "{\"jsonrpc\":\"2.0\",\"id\":42,"
                      "\"method\":\"item/commandExecution/requestApproval\",\"params\":{}}";
   char buf[96];
   assert(cli_codex_parse_server_request_method(line, buf, sizeof(buf)) == 1);
   assert(strcmp(buf, "item/commandExecution/requestApproval") == 0);
}

static void test_parse_server_request_method_extracts_legacy_approval(void)
{
   const char *line = "{\"jsonrpc\":\"2.0\",\"id\":\"req-1\","
                      "\"method\":\"execCommandApproval\",\"params\":{}}";
   char buf[64];
   assert(cli_codex_parse_server_request_method(line, buf, sizeof(buf)) == 1);
   assert(strcmp(buf, "execCommandApproval") == 0);
}

static void test_parse_server_request_method_skips_notifications_and_responses(void)
{
   char buf[64] = "untouched";
   assert(cli_codex_parse_server_request_method(
              "{\"jsonrpc\":\"2.0\",\"method\":\"item/completed\",\"params\":{}}", buf,
              sizeof(buf)) == 0);
   assert(strcmp(buf, "untouched") == 0);
   assert(cli_codex_parse_server_request_method("{\"jsonrpc\":\"2.0\",\"id\":1,"
                                                "\"result\":{}}",
                                                buf, sizeof(buf)) == 0);
   assert(strcmp(buf, "untouched") == 0);
}

static void test_parse_server_request_method_invalid_inputs(void)
{
   char buf[64];
   assert(cli_codex_parse_server_request_method(NULL, buf, sizeof(buf)) == 0);
   assert(cli_codex_parse_server_request_method("{\"id\":1,\"method\":\"x\"}", NULL, sizeof(buf)) ==
          0);
   assert(cli_codex_parse_server_request_method("{\"id\":1,\"method\":\"x\"}", buf, 0) == 0);
   assert(cli_codex_parse_server_request_method("not json", buf, sizeof(buf)) == 0);
}

static void test_approval_decision_respects_autonomous_mode(void)
{
   char buf[32];
   assert(cli_codex_approval_decision("item/commandExecution/requestApproval", 0, buf,
                                      sizeof(buf)) == 1);
   assert(strcmp(buf, "decline") == 0);
   assert(cli_codex_approval_decision("item/commandExecution/requestApproval", 1, buf,
                                      sizeof(buf)) == 1);
   assert(strcmp(buf, "accept") == 0);
}

static void test_approval_decision_uses_legacy_review_values(void)
{
   char buf[32];
   assert(cli_codex_approval_decision("execCommandApproval", 0, buf, sizeof(buf)) == 1);
   assert(strcmp(buf, "denied") == 0);
   assert(cli_codex_approval_decision("execCommandApproval", 1, buf, sizeof(buf)) == 1);
   assert(strcmp(buf, "approved") == 0);
}

static void test_approval_decision_skips_unknown_methods(void)
{
   char buf[32] = "untouched";
   assert(cli_codex_approval_decision("item/tool/requestUserInput", 1, buf, sizeof(buf)) == 0);
   assert(strcmp(buf, "untouched") == 0);
   assert(cli_codex_approval_decision(NULL, 1, buf, sizeof(buf)) == 0);
   assert(cli_codex_approval_decision("execCommandApproval", 1, NULL, sizeof(buf)) == 0);
   assert(cli_codex_approval_decision("execCommandApproval", 1, buf, 0) == 0);
}

static void test_server_restart_command_detection_blocks_lifecycle_writes(void)
{
   assert(cli_codex_command_restarts_aimee_server("./aimee server restart") == 1);
   assert(cli_codex_command_restarts_aimee_server("systemctl --user restart aimee-server") == 1);
   assert(cli_codex_command_restarts_aimee_server("pkill -f aimee-server") == 1);
}

static void test_server_restart_command_detection_allows_read_only_checks(void)
{
   assert(cli_codex_command_restarts_aimee_server("systemctl --user status aimee-server") == 0);
   assert(cli_codex_command_restarts_aimee_server("aimee status") == 0);
   assert(cli_codex_command_restarts_aimee_server("make -C src unit-tests") == 0);
}

static void test_spawn_failure_captures_stderr_and_exit_status(void)
{
   char path[256];
   snprintf(path, sizeof path, "%s/aimee-codex-fixture-XXXXXX", platform_tmpdir());
   int fd = mkstemp(path);
   assert(fd >= 0);
   const char *script =
       "#!/bin/sh\necho fixture-args:$* >&2\necho fixture-stderr-message >&2\nexit 42\n";
   assert(write(fd, script, strlen(script)) == (ssize_t)strlen(script));
   close(fd);
   assert(chmod(path, 0700) == 0);

   agent_t agent = {0};
   snprintf(agent.name, sizeof(agent.name), "codex");
   snprintf(agent.cli_cmd, sizeof(agent.cli_cmd), "%s", path);
   agent.autonomous = 1;

   agent_result_t out;
   int rc = agent_execute_cli_codex(&agent, NULL, "hello", &out);
   assert(rc == -1);
   assert(strstr(out.error, "status 42") != NULL);
   assert(strstr(out.error, "approval_policy=\"on-request\"") != NULL);
   assert(strstr(out.error, "sandbox_mode=\"read-only\"") != NULL);
   assert(strstr(out.error, "fixture-stderr-message") != NULL);

   agent.write_capable = 1;
   rc = agent_execute_cli_codex(&agent, NULL, "hello", &out);
   assert(rc == -1);
   assert(strstr(out.error, "sandbox_mode=\"workspace-write\"") != NULL);
   unlink(path);
}

static void test_parse_tool_action_counts_non_agent_items(void)
{
   /* Any item/completed whose type isn't agentMessage is a tool action
    * (shell exec, file edit, web fetch, ...). Counting these unblocks
    * the "1 turn 0 tools" reporting bug — tool_calls was always 0. */
   const char *shell = "{\"jsonrpc\":\"2.0\",\"method\":\"item/completed\",\"params\":{"
                       "\"item\":{\"type\":\"localShellCall\",\"command\":\"ls\"}}}";
   const char *edit = "{\"jsonrpc\":\"2.0\",\"method\":\"item/completed\",\"params\":{"
                      "\"item\":{\"type\":\"fileEdit\",\"path\":\"foo.c\"}}}";
   const char *fetch = "{\"jsonrpc\":\"2.0\",\"method\":\"item/completed\",\"params\":{"
                       "\"item\":{\"type\":\"webFetch\",\"url\":\"x\"}}}";
   assert(cli_codex_parse_tool_action(shell) == 1);
   assert(cli_codex_parse_tool_action(edit) == 1);
   assert(cli_codex_parse_tool_action(fetch) == 1);
}

static void test_parse_tool_action_skips_agent_message(void)
{
   const char *agent = "{\"jsonrpc\":\"2.0\",\"method\":\"item/completed\",\"params\":{"
                       "\"item\":{\"type\":\"agentMessage\",\"text\":\"hi\"}}}";
   assert(cli_codex_parse_tool_action(agent) == 0);
}

static void test_parse_tool_action_skips_other_methods(void)
{
   const char *line = "{\"jsonrpc\":\"2.0\",\"method\":\"turn/started\",\"params\":{}}";
   assert(cli_codex_parse_tool_action(line) == 0);
   assert(cli_codex_parse_tool_action("not json") == 0);
   assert(cli_codex_parse_tool_action(NULL) == 0);
}

static int bind_native_fixture(const char *id, void *context)
{
   assert(!strcmp(id, "t1"));
   (*(int *)context)++;
   return 0;
}

static void test_agent_execute_cli_codex_uses_requested_cwd(void)
{
   char root[256];
   snprintf(root, sizeof root, "%s/aimee-codex-cwd-XXXXXX", platform_tmpdir());
   assert(mkdtemp(root) != NULL);

   char workdir[512];
   char script[512];
   char marker[512];
   char request_marker[512];
   snprintf(workdir, sizeof(workdir), "%s/work", root);
   snprintf(script, sizeof(script), "%s/fake-codex", root);
   snprintf(marker, sizeof(marker), "%s/cwd.txt", root);
   snprintf(request_marker, sizeof(request_marker), "%s/request.txt", root);
   assert(mkdir(workdir, 0700) == 0);

   FILE *fp = fopen(script, "w");
   assert(fp != NULL);
   fputs("#!/bin/sh\n"
         "pwd > \"$AIMEE_TEST_CODEX_CWD_OUT\"\n"
         "if [ -n \"$AIMEE_TEST_CODEX_RESUME_OUT\" ]; then\n"
         "  printf '%s\\n' \"$HOME\" \"$AIMEE_SESSION_ID\" > \"$AIMEE_TEST_CODEX_RESUME_OUT\"\n"
         "fi\n"
         "i=0\n"
         "while IFS= read -r line; do\n"
         "  i=$((i + 1))\n"
         "  case \"$i\" in\n"
         "    1) printf '%s\\n' '{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{}}' ;;\n"
         "    2) printf '%s\\n' \"$line\" > \"$AIMEE_TEST_CODEX_REQUEST_OUT\"\n"
         "       if [ -n \"$AIMEE_TEST_CODEX_RESUME_OUT\" ]; then printf '%s\\n' \"$line\" >> "
         "\"$AIMEE_TEST_CODEX_RESUME_OUT\"; fi\n"
         "       printf '%s\\n' "
         "'{\"jsonrpc\":\"2.0\",\"id\":2,\"result\":{\"thread\":{\"id\":\"t1\"}}}' ;;\n"
         "    3) printf '%s\\n' \"$line\" > \"$AIMEE_TEST_CODEX_REQUEST_OUT\"\n"
         "       printf '%s\\n' '{\"jsonrpc\":\"2.0\",\"id\":3,\"result\":{}}'\n"
         "       printf '%s\\n' "
         "'{\"jsonrpc\":\"2.0\",\"method\":\"item/completed\",\"params\":{\"item\":"
         "{\"type\":\"agentMessage\",\"text\":\"ok from fake codex\"}}}'\n"
         "       printf '%s\\n' "
         "'{\"jsonrpc\":\"2.0\",\"method\":\"turn/completed\",\"params\":{\"turn\":"
         "{\"status\":\"completed\"}}}'\n"
         "       exit 0 ;;\n"
         "  esac\n"
         "done\n",
         fp);
   fclose(fp);
   assert(chmod(script, 0700) == 0);
   assert(setenv("AIMEE_TEST_CODEX_CWD_OUT", marker, 1) == 0);
   assert(setenv("AIMEE_TEST_CODEX_REQUEST_OUT", request_marker, 1) == 0);

   agent_t agent;
   memset(&agent, 0, sizeof(agent));
   snprintf(agent.name, sizeof(agent.name), "fake-codex");
   snprintf(agent.cli_cmd, sizeof(agent.cli_cmd), "%s", script);
   agent.cli_idle_timeout_ms = 5000;

   agent_result_t out;
   assert(agent_execute_cli_codex_at_cwd(&agent, workdir, "system", "user prompt", &out) == 0);
   assert(out.response != NULL);
   assert(strcmp(out.response, "ok from fake codex") == 0);
   free(out.response);

   char seen[512];
   fp = fopen(marker, "r");
   assert(fp != NULL);
   assert(fgets(seen, sizeof(seen), fp) != NULL);
   fclose(fp);
   seen[strcspn(seen, "\r\n")] = '\0';
   assert(strcmp(seen, workdir) == 0);

   fp = fopen(request_marker, "r");
   assert(fp != NULL);
   char req_line[2048];
   assert(fgets(req_line, sizeof(req_line), fp) != NULL);
   fclose(fp);
   assert(strstr(req_line, "\"type\":\"text\"") != NULL);
   assert(strstr(req_line, "\"text\":\"user prompt\"") != NULL);
   assert(strstr(req_line, "\"text_elements\":[]") != NULL);

   char resume_marker[512];
   snprintf(resume_marker, sizeof resume_marker, "%s/resume.txt", root);
   setenv("AIMEE_TEST_CODEX_RESUME_OUT", resume_marker, 1);
   int bound = 0;
   provider_cli_cfg_t cfg = {.agent = &agent,
                             .cwd = workdir,
                             .system_prompt = "system",
                             .user_prompt = "resumed prompt",
                             .home = workdir,
                             .session_id = "private-delegate",
                             .resume_id = "t1",
                             .bind_session = bind_native_fixture,
                             .bind_context = &bound};
   assert(!codex_provider_cli_adapter.execute(&cfg, &out));
   assert(out.success && bound == 1);
   free(out.response);
   fp = fopen(resume_marker, "r");
   assert(fp && fgets(seen, sizeof seen, fp));
   seen[strcspn(seen, "\r\n")] = 0;
   assert(!strcmp(seen, workdir));
   assert(fgets(seen, sizeof seen, fp) && !strcmp(seen, "private-delegate\n"));
   assert(fgets(req_line, sizeof req_line, fp));
   assert(strstr(req_line, "thread/resume") && strstr(req_line, "t1"));
   fclose(fp);
   unsetenv("AIMEE_TEST_CODEX_RESUME_OUT");
   unlink(resume_marker);

   unsetenv("AIMEE_TEST_CODEX_CWD_OUT");
   unsetenv("AIMEE_TEST_CODEX_REQUEST_OUT");
   unlink(marker);
   unlink(request_marker);
   unlink(script);
   rmdir(workdir);
   rmdir(root);
}

typedef struct
{
   char buf[512];
} codex_stream_capture_t;

static void codex_capture_append(codex_stream_capture_t *cap, const char *text)
{
   size_t used = strlen(cap->buf);
   size_t add = strlen(text);
   assert(used + add < sizeof(cap->buf));
   memcpy(cap->buf + used, text, add + 1);
}

static int codex_capture_cb(const char *event, const char *value, void *userdata)
{
   codex_stream_capture_t *cap = (codex_stream_capture_t *)userdata;
   if (strcmp(event, "turn_start") == 0)
      codex_capture_append(cap, "<S>");
   else if (strcmp(event, "turn_end") == 0)
      codex_capture_append(cap, "<E>");
   else if (strcmp(event, "text") == 0)
      codex_capture_append(cap, value ? value : "");
   return 0;
}

static void test_chat_stream_splits_distinct_agent_message_items(void)
{
   char root[256];
   snprintf(root, sizeof root, "%s/aimee-codex-stream-XXXXXX", platform_tmpdir());
   assert(mkdtemp(root) != NULL);

   char script[512];
   snprintf(script, sizeof(script), "%s/codex", root);
   FILE *fp = fopen(script, "w");
   assert(fp != NULL);
   fputs("#!/bin/sh\n"
         "i=0\n"
         "while IFS= read -r line; do\n"
         "  i=$((i + 1))\n"
         "  case \"$i\" in\n"
         "    1) printf '%s\\n' '{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{}}' ;;\n"
         "    2) printf '%s\\n' "
         "'{\"jsonrpc\":\"2.0\",\"id\":2,\"result\":{\"thread\":{\"id\":\"t1\"}}}' ;;\n"
         "    3) printf '%s\\n' '{\"jsonrpc\":\"2.0\",\"id\":3,\"result\":{}}'\n"
         "       printf '%s\\n' "
         "'{\"jsonrpc\":\"2.0\",\"method\":\"item/agentMessage/delta\",\"params\":"
         "{\"threadId\":\"t1\",\"turnId\":\"turn1\",\"itemId\":\"m1\",\"delta\":\"First.\"}}'\n"
         "       printf '%s\\n' "
         "'{\"jsonrpc\":\"2.0\",\"method\":\"item/completed\",\"params\":{\"item\":"
         "{\"type\":\"agentMessage\",\"id\":\"m1\",\"text\":\"First.\","
         "\"phase\":\"commentary\"}}}'\n"
         "       printf '%s\\n' "
         "'{\"jsonrpc\":\"2.0\",\"method\":\"item/agentMessage/delta\",\"params\":"
         "{\"threadId\":\"t1\",\"turnId\":\"turn1\",\"itemId\":\"m2\",\"delta\":\"Second.\"}}'\n"
         "       printf '%s\\n' "
         "'{\"jsonrpc\":\"2.0\",\"method\":\"item/completed\",\"params\":{\"item\":"
         "{\"type\":\"agentMessage\",\"id\":\"m2\",\"text\":\"Second.\","
         "\"phase\":\"final_answer\"}}}'\n"
         "       printf '%s\\n' "
         "'{\"jsonrpc\":\"2.0\",\"method\":\"turn/completed\",\"params\":{\"turn\":"
         "{\"status\":\"completed\"}}}'\n"
         "       exit 0 ;;\n"
         "  esac\n"
         "done\n",
         fp);
   fclose(fp);
   assert(chmod(script, 0700) == 0);

   const char *old_path_env = getenv("PATH");
   char old_path[8192] = "";
   if (old_path_env)
      snprintf(old_path, sizeof(old_path), "%s", old_path_env);
   char new_path[8704];
   snprintf(new_path, sizeof(new_path), "%s:%s", root, old_path_env ? old_path_env : "");
   assert(setenv("PATH", new_path, 1) == 0);

   cli_codex_chat_request_t req = {0};
   req.cwd = root;
   req.user_prompt = "hello";
   req.timeout_ms = 5000;

   cli_codex_chat_result_t out;
   codex_stream_capture_t cap = {0};
   assert(cli_codex_chat_stream(&req, codex_capture_cb, &cap, &out) == 0);
   assert(strcmp(cap.buf, "<S>First.<S>Second.<E>") == 0);

   if (old_path_env)
      assert(setenv("PATH", old_path, 1) == 0);
   else
      unsetenv("PATH");
   unlink(script);
   rmdir(root);
}

static void test_chat_stream_splits_completed_delta_items_without_ids(void)
{
   char root[256];
   snprintf(root, sizeof root, "%s/aimee-codex-stream-noids-XXXXXX", platform_tmpdir());
   assert(mkdtemp(root) != NULL);

   char script[512];
   snprintf(script, sizeof(script), "%s/codex", root);
   FILE *fp = fopen(script, "w");
   assert(fp != NULL);
   fputs("#!/bin/sh\n"
         "i=0\n"
         "while IFS= read -r line; do\n"
         "  i=$((i + 1))\n"
         "  case \"$i\" in\n"
         "    1) printf '%s\\n' '{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{}}' ;;\n"
         "    2) printf '%s\\n' "
         "'{\"jsonrpc\":\"2.0\",\"id\":2,\"result\":{\"thread\":{\"id\":\"t1\"}}}' ;;\n"
         "    3) printf '%s\\n' '{\"jsonrpc\":\"2.0\",\"id\":3,\"result\":{}}'\n"
         "       printf '%s\\n' "
         "'{\"jsonrpc\":\"2.0\",\"method\":\"item/agentMessage/delta\",\"params\":"
         "{\"threadId\":\"t1\",\"turnId\":\"turn1\",\"delta\":\"First.\"}}'\n"
         "       printf '%s\\n' "
         "'{\"jsonrpc\":\"2.0\",\"method\":\"item/completed\",\"params\":{\"item\":"
         "{\"type\":\"agentMessage\",\"text\":\"First.\",\"phase\":\"commentary\"}}}'\n"
         "       printf '%s\\n' "
         "'{\"jsonrpc\":\"2.0\",\"method\":\"item/agentMessage/delta\",\"params\":"
         "{\"threadId\":\"t1\",\"turnId\":\"turn1\",\"delta\":\"Second.\"}}'\n"
         "       printf '%s\\n' "
         "'{\"jsonrpc\":\"2.0\",\"method\":\"item/completed\",\"params\":{\"item\":"
         "{\"type\":\"agentMessage\",\"text\":\"Second.\",\"phase\":\"final_answer\"}}}'\n"
         "       printf '%s\\n' "
         "'{\"jsonrpc\":\"2.0\",\"method\":\"turn/completed\",\"params\":{\"turn\":"
         "{\"status\":\"completed\"}}}'\n"
         "       exit 0 ;;\n"
         "  esac\n"
         "done\n",
         fp);
   fclose(fp);
   assert(chmod(script, 0700) == 0);

   const char *old_path_env = getenv("PATH");
   char old_path[8192] = "";
   if (old_path_env)
      snprintf(old_path, sizeof(old_path), "%s", old_path_env);
   char new_path[8704];
   snprintf(new_path, sizeof(new_path), "%s:%s", root, old_path_env ? old_path_env : "");
   assert(setenv("PATH", new_path, 1) == 0);

   cli_codex_chat_request_t req = {0};
   req.cwd = root;
   req.user_prompt = "hello";
   req.timeout_ms = 5000;

   cli_codex_chat_result_t out;
   codex_stream_capture_t cap = {0};
   assert(cli_codex_chat_stream(&req, codex_capture_cb, &cap, &out) == 0);
   assert(strcmp(cap.buf, "<S>First.<S>Second.<E>") == 0);

   if (old_path_env)
      assert(setenv("PATH", old_path, 1) == 0);
   else
      unsetenv("PATH");
   unlink(script);
   rmdir(root);
}

static void test_chat_stream_replaces_invalid_utf8_prompt_bytes(void)
{
   char root[256];
   snprintf(root, sizeof root, "%s/aimee-codex-utf8-XXXXXX", platform_tmpdir());
   assert(mkdtemp(root) != NULL);

   char script[512];
   char marker[512];
   snprintf(script, sizeof(script), "%s/codex", root);
   snprintf(marker, sizeof(marker), "%s/request.jsonl", root);
   FILE *fp = fopen(script, "w");
   assert(fp != NULL);
   fputs("#!/bin/sh\n"
         "i=0\n"
         "while IFS= read -r line; do\n"
         "  i=$((i + 1))\n"
         "  case \"$i\" in\n"
         "    1) printf '%s\\n' '{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{}}' ;;\n"
         "    2) printf '%s\\n' "
         "'{\"jsonrpc\":\"2.0\",\"id\":2,\"result\":{\"thread\":{\"id\":\"t1\"}}}' ;;\n"
         "    3) printf '%s' \"$line\" > \"$AIMEE_TEST_CODEX_REQUEST_OUT\"\n"
         "       printf '%s\\n' '{\"jsonrpc\":\"2.0\",\"id\":3,\"result\":{}}'\n"
         "       printf '%s\\n' "
         "'{\"jsonrpc\":\"2.0\",\"method\":\"item/completed\",\"params\":{\"item\":"
         "{\"type\":\"agentMessage\",\"text\":\"ok\"}}}'\n"
         "       printf '%s\\n' "
         "'{\"jsonrpc\":\"2.0\",\"method\":\"turn/completed\",\"params\":{\"turn\":"
         "{\"status\":\"completed\"}}}'\n"
         "       exit 0 ;;\n"
         "  esac\n"
         "done\n",
         fp);
   fclose(fp);
   assert(chmod(script, 0700) == 0);

   const char *old_path_env = getenv("PATH");
   char old_path[8192] = "";
   if (old_path_env)
      snprintf(old_path, sizeof(old_path), "%s", old_path_env);
   char new_path[8704];
   snprintf(new_path, sizeof(new_path), "%s:%s", root, old_path_env ? old_path_env : "");
   assert(setenv("PATH", new_path, 1) == 0);
   assert(setenv("AIMEE_TEST_CODEX_REQUEST_OUT", marker, 1) == 0);

   const char bad_prompt[] = {'b',        'a',        'd',        ' ',        (char)0x80, ' ',
                              (char)0xf0, (char)0x28, (char)0x8c, (char)0x28, '\0'};
   cli_codex_chat_request_t req = {0};
   req.cwd = root;
   req.user_prompt = bad_prompt;
   req.timeout_ms = 5000;

   cli_codex_chat_result_t out;
   codex_stream_capture_t cap = {0};
   assert(cli_codex_chat_stream(&req, codex_capture_cb, &cap, &out) == 0);
   assert(strcmp(cap.buf, "<S>ok<E>") == 0);

   unsigned char buf[4096];
   fp = fopen(marker, "rb");
   assert(fp != NULL);
   size_t n = fread(buf, 1, sizeof(buf), fp);
   fclose(fp);
   assert(n > 0 && n < sizeof(buf));

   int saw_replacement = 0;
   for (size_t i = 0; i < n; i++)
   {
      assert(buf[i] != 0x80);
      if (i + 2 < n && buf[i] == 0xef && buf[i + 1] == 0xbf && buf[i + 2] == 0xbd)
         saw_replacement = 1;
   }
   assert(saw_replacement);

   unsetenv("AIMEE_TEST_CODEX_REQUEST_OUT");
   if (old_path_env)
      assert(setenv("PATH", old_path, 1) == 0);
   else
      unsetenv("PATH");
   unlink(marker);
   unlink(script);
   rmdir(root);
}

static int operator_requests, operator_responses;
static int codex_operator_capture(const aimee_session_event_t *event, void *context)
{
   (void)context;
   if (event->kind == AIMEE_SESSION_CONTROL_REQUEST)
   {
      server_control_scope_t *scope = server_control_scope_current();
      assert(server_control_register(scope, event->request_id, event->control_kind,
                                     event->control) == 0);
      cJSON *reply =
          cJSON_Parse(!strcmp(event->control_kind, "question")
                          ? "{\"decision\":\"allow\",\"answers\":{\"direction\":[\"North\"]}}"
                      : !strcmp(event->control_kind, "elicitation") ? "{\"decision\":\"allow\"}"
                                                                    : "{\"decision\":\"cancel\"}");
      char id[256];
      snprintf(id, sizeof id, "operator-turn:%s", event->request_id);
      assert(server_control_reply("operator-session", id, "alice", reply) == 0);
      cJSON_Delete(reply);
      operator_requests++;
   }
   else if (event->kind == AIMEE_SESSION_CONTROL_RESPONSE)
   {
      const char *decision =
          cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(event->control, "decision"));
      assert(decision &&
             !strcmp(decision, !strcmp(event->control_kind, "permission") ? "cancel" : "allow"));
      if (!strcmp(event->control_kind, "question"))
         assert(cJSON_IsArray(cJSON_GetObjectItemCaseSensitive(
             cJSON_GetObjectItemCaseSensitive(event->control, "answers"), "direction")));
      assert(!cJSON_GetObjectItemCaseSensitive(event->control, "action"));
      operator_responses++;
   }
   return 0;
}
static void test_operator_replies_reach_native_child(void)
{
   char root[256], script[512];
   snprintf(root, sizeof root, "%s/aimee-codex-operator-XXXXXX", platform_tmpdir());
   assert(mkdtemp(root));
   snprintf(script, sizeof script, "%s/codex", root);
   FILE *fp = fopen(script, "w");
   assert(fp);
   fputs("#!/usr/bin/env python3\nimport json, sys\ndef send(value):\n print(json.dumps(value), "
         "flush=True)\nfor i in range(3):\n frame=json.loads(sys.stdin.readline())\n if "
         "frame.get('method') == 'initialized':\n  frame=json.loads(sys.stdin.readline())\n "
         "result={'thread': {'id':'t1'}} if frame['method'].startswith('thread/') else {}\n "
         "send({'jsonrpc':'2.0','id':frame['id'],'result':result})\nfor method, ident, params, "
         "expected in [\n ('item/commandExecution/requestApproval', 71, {'command':'printf "
         "hello'}, {'decision':'decline'}),\n ('item/tool/requestUserInput', 72, "
         "{'questions':[{'id':'direction','question':'Which "
         "direction?','options':[{'label':'North'}]}]}, "
         "{'answers':{'direction':{'answers':['North']}}}),\n"
         " ('mcpServer/elicitation/request', 73, {'mode':'url','serverName':'inventory',"
         "'message':'Authorize access','url':'https://accounts.example.test/authorize',"
         "'elicitationId':'el-1'}, {'action':'accept','content':None,'_meta':None})\n]:\n "
         "send({'jsonrpc':'2.0','id':ident,'method':method,'params':params})\n "
         "frame=json.loads(sys.stdin.readline())\n assert frame['id']==ident and "
         "frame['result']==expected, "
         "frame\nsend({'method':'item/"
         "completed','params':{'item':{'type':'agentMessage','id':'m1','text':'Operator replies "
         "received'}}})\nsend({'method':'turn/"
         "completed','params':{'turn':{'status':'completed'}}})\n",
         fp);
   assert(fclose(fp) == 0 && chmod(script, 0700) == 0);
   char *old_path = getenv("PATH") ? strdup(getenv("PATH")) : NULL;
   char path[16384];
   snprintf(path, sizeof path, "%s:%s", root, old_path ? old_path : "");
   assert(setenv("PATH", path, 1) == 0);
   server_control_scope_t *scope =
       server_control_scope_open("operator-session", "operator-turn", "alice");
   assert(scope);
   server_control_scope_t *previous = server_control_scope_bind(scope);
   aimee_ir_session_set_sink(codex_operator_capture, NULL);
   cli_codex_chat_request_t req = {.cwd = root, .user_prompt = "hello", .timeout_ms = 5000};
   cli_codex_chat_result_t out;
   codex_stream_capture_t cap = {0};
   assert(cli_codex_chat_stream(&req, codex_capture_cb, &cap, &out) == 0);
   assert(strstr(cap.buf, "Operator replies received"));
   assert(operator_requests == 3 && operator_responses == 3);
   aimee_ir_session_set_sink(NULL, NULL);
   server_control_scope_bind(previous);
   server_control_scope_close(scope);
   if (old_path)
      assert(setenv("PATH", old_path, 1) == 0);
   else
      unsetenv("PATH");
   free(old_path);
   unlink(script);
   rmdir(root);
}

static void test_canonical_history_adoption(void)
{
   char root[256], script[512], marker[512];
   snprintf(root, sizeof root, "%s/aimee-codex-adoption-XXXXXX", platform_tmpdir());
   assert(mkdtemp(root));
   snprintf(script, sizeof script, "%s/codex", root);
   snprintf(marker, sizeof marker, "%s/requests", root);
   FILE *fp = fopen(script, "w");
   assert(fp);
   fputs("#!/usr/bin/env python3\nimport json, os, sys\n"
         "def send(v):\n print(json.dumps(v),flush=True)\n"
         "adopted=False\nlast=0\n"
         "for line in sys.stdin:\n f=json.loads(line)\n m=f.get('method')\n"
         " if m=='initialized': continue\n"
         " with open(os.environ['AIMEE_TEST_HISTORY_LOG'],'a') as log: "
         "log.write(json.dumps(f)+'\\n')\n"
         " assert f['id']>last\n last=f['id']\n p=f.get('params',{})\n"
         " if m=='initialize': send({'id':f['id'],'result':{}})\n"
         " elif m in ('thread/start','thread/resume'):\n"
         "  assert m==os.environ['AIMEE_TEST_HISTORY_SESSION']\n"
         "  send({'id':f['id'],'result':{'thread':{'id':'t-history'}}})\n"
         " elif m=='thread/inject_items':\n"
         "  assert p['threadId']=='t-history'\n  a=p['items']\n"
         "  assert len(a)==5, a\n"
         "  assert a[0]['role']=='user' and a[0]['content'][0]['text']=='prior user'\n"
         "  assert a[1]['type']=='reasoning' and a[1]['id']=='rs_fixture'\n"
         "  assert a[1]['encrypted_content']=='synthetic-opaque-history'\n"
         "  assert a[2]['type']=='function_call' and a[2]['call_id']=='call:'+('x'*600)\n"
         "  assert a[2]['name']=='inspect' and json.loads(a[2]['arguments'])=={'path':'docs'}\n"
         "  assert a[3]['type']=='function_call_output' and a[3]['call_id']==a[2]['call_id']\n"
         "  assert a[3]['output']=='completed'\n"
         "  assert a[4]['role']=='assistant' and a[4]['content'][0]['text']=='prior assistant'\n"
         "  if os.environ['AIMEE_TEST_HISTORY_REFUSE']=='1':\n"
         "   send({'id':f['id'],'error':{'code':-32601,'message':'unsupported'}})\n"
         "  else:\n   adopted=True\n   send({'id':f['id'],'result':{}})\n"
         " elif m=='turn/start':\n"
         "  assert adopted and os.environ['AIMEE_TEST_HISTORY_REFUSE']=='0'\n"
         "  assert p['input']==[{'type':'text','text':'current user','text_elements':[]}]\n"
         "  send({'id':f['id'],'result':{}})\n"
         "  send({'method':'item/completed','params':{'item':{'type':'agentMessage',"
         "'id':'answer','text':'history adopted'}}})\n"
         "  send({'method':'turn/completed','params':{'turn':{'status':'completed'}}})\n"
         " else: raise AssertionError(m)\n",
         fp);
   assert(fclose(fp) == 0 && chmod(script, 0700) == 0);
   char *old_path = getenv("PATH") ? strdup(getenv("PATH")) : NULL;
   char path[16384];
   snprintf(path, sizeof path, "%s:%s", root, old_path ? old_path : "");
   assert(!setenv("PATH", path, 1));
   assert(!setenv("AIMEE_TEST_HISTORY_LOG", marker, 1));
   char call_id[606] = "call:";
   memset(call_id + 5, 'x', 600);
   call_id[605] = 0;
   cJSON *args = cJSON_Parse("{\"path\":\"docs\"}");
   cJSON *result = cJSON_CreateString("completed");
   aimee_block_t blocks[] = {
       {.type = AIMEE_BLK_TEXT, .text = "prior user"},
       {.type = AIMEE_BLK_THINKING,
        .text = "summary",
        .thinking_id = "rs_fixture",
        .thinking_encrypted_content = "synthetic-opaque-history"},
       {.type = AIMEE_BLK_TOOL_USE, .tool_id = call_id, .tool_name = "inspect", .tool_input = args},
       {.type = AIMEE_BLK_TOOL_RESULT, .tool_id = call_id, .tool_result = result},
       {.type = AIMEE_BLK_TEXT, .text = "prior assistant"}};
   aimee_message_t messages[] = {{.role = "user", .blocks = blocks, .n_blocks = 1},
                                 {.role = "assistant", .blocks = blocks + 1, .n_blocks = 2},
                                 {.role = "user", .blocks = blocks + 3, .n_blocks = 1},
                                 {.role = "assistant", .blocks = blocks + 4, .n_blocks = 1}};
   aimee_request_t history = {.messages = messages, .n_messages = 4};
   agent_t agent = {0};
   snprintf(agent.name, sizeof agent.name, "history-fixture");
   snprintf(agent.cli_cmd, sizeof agent.cli_cmd, "%s", script);
   agent.cli_idle_timeout_ms = 5000;
   for (int provider = 0; provider < 2; provider++)
      for (int resume = 0; resume < 2; resume++)
         for (int refuse = 0; refuse < 3; refuse++)
         {
            unlink(marker);
            setenv("AIMEE_TEST_HISTORY_SESSION", resume ? "thread/resume" : "thread/start", 1);
            setenv("AIMEE_TEST_HISTORY_REFUSE", refuse == 1 ? "1" : "0", 1);
            /* Invalid canonical content must refuse before injection/turn/start. */
            blocks[0].type = refuse == 2 ? AIMEE_BLK_UNKNOWN : AIMEE_BLK_TEXT;
            int rc;
            if (provider)
            {
               provider_cli_cfg_t cfg = {.agent = &agent,
                                         .cwd = root,
                                         .history = &history,
                                         .user_prompt = "current user",
                                         .resume_id = resume ? "t-history" : NULL};
               agent_result_t out;
               rc = codex_provider_cli_adapter.execute(&cfg, &out);
               if (!refuse)
                  assert(out.success && out.response && !strcmp(out.response, "history adopted"));
               else
                  assert(strstr(out.error, "canonical history adoption failed"));
               free(out.response);
            }
            else
            {
               cli_codex_chat_request_t req = {.cwd = root,
                                               .history = &history,
                                               .user_prompt = "current user",
                                               .thread_id = resume ? "t-history" : NULL,
                                               .timeout_ms = 5000};
               cli_codex_chat_result_t out;
               codex_stream_capture_t cap = {0};
               rc = cli_codex_chat_stream(&req, codex_capture_cb, &cap, &out);
               if (!refuse && rc)
                  fprintf(stderr, "adoption rc=%d error=%s\n", rc, out.error);
               if (!refuse)
                  assert(strstr(cap.buf, "history adopted"));
               else
                  assert(strstr(out.error, "canonical history adoption failed"));
            }
            assert(refuse ? rc != 0 : rc == 0);
            fp = fopen(marker, "r");
            assert(fp);
            char log[16384] = {0};
            size_t n = fread(log, 1, sizeof log - 1, fp);
            assert(n && !ferror(fp));
            fclose(fp);
            assert((strstr(log, "thread/inject_items") != NULL) == (refuse != 2));
            assert((strstr(log, "turn/start") != NULL) == !refuse);
         }
   blocks[0].type = AIMEE_BLK_TEXT;
   assert(!strcmp(blocks[1].thinking_encrypted_content, "synthetic-opaque-history"));
   assert(!strcmp(blocks[2].tool_id, call_id));
   cJSON_Delete(args);
   cJSON_Delete(result);
   if (old_path)
      setenv("PATH", old_path, 1);
   else
      unsetenv("PATH");
   free(old_path);
   unsetenv("AIMEE_TEST_HISTORY_LOG");
   unsetenv("AIMEE_TEST_HISTORY_SESSION");
   unsetenv("AIMEE_TEST_HISTORY_REFUSE");
   unlink(marker);
   unlink(script);
   rmdir(root);
}

/* Used by the opt-in real SDK integration fixture. All model traffic is sent
 * to its loopback provider; no server/Vault credential is needed or read. */
static int native_probe_capture(const aimee_session_event_t *event, void *context)
{
   cJSON *json = aimee_ir_session_event_json(event);
   return json && cJSON_AddItemToArray(context, json) ? 0 : -1;
}

static void test_dynamic_tool_boundaries(void)
{
   cJSON *schema =
       cJSON_Parse("{\"type\":\"object\",\"properties\":{\"marker\":{\"type\":\"number\"}}}");
   aimee_tool_t child = {.name = "exec_command", .description = "host only", .schema = schema};
   aimee_tool_t space = {.kind = AIMEE_TOOL_NAMESPACE,
                         .name = "aimee",
                         .description = "host",
                         .children = &child,
                         .n_children = 1};
   aimee_request_t catalog = {.tools = &space, .n_tools = 1};
   cJSON *tools = cli_codex_dynamic_tools(&catalog);
   assert(tools && cJSON_GetArraySize(tools) == 1);
   cJSON *definition =
       cJSON_GetArrayItem(cJSON_GetObjectItem(cJSON_GetArrayItem(tools, 0), "tools"), 0);
   assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(definition, "name")), "exec_command"));
   assert(cJSON_Compare(cJSON_GetObjectItem(definition, "inputSchema"), schema, 1));
   cJSON_Delete(tools);
   /* No raw sidecar can replace the typed name/schema or grant a hosted tool. */
   child.raw = cJSON_Parse("{\"name\":\"forged\",\"inputSchema\":false}");
   tools = cli_codex_dynamic_tools(&catalog);
   assert(tools);
   definition = cJSON_GetArrayItem(cJSON_GetObjectItem(cJSON_GetArrayItem(tools, 0), "tools"), 0);
   assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(definition, "name")), "exec_command"));
   assert(cJSON_Compare(cJSON_GetObjectItem(definition, "inputSchema"), schema, 1));
   cJSON_Delete(tools);
   child.strict = AIMEE_STRICT_TRUE;
   assert(!cli_codex_dynamic_tools(&catalog));
   child.strict = AIMEE_STRICT_UNSET;
   child.cache_control = "ephemeral";
   assert(!cli_codex_dynamic_tools(&catalog));
   child.cache_control = NULL;
   child.kind = AIMEE_TOOL_HOSTED;
   assert(!cli_codex_dynamic_tools(&catalog));
   child.kind = AIMEE_TOOL_FUNCTION;
   cJSON *params =
       cJSON_Parse("{\"threadId\":\"thread\",\"turnId\":\"turn\",\"callId\":\"call:original\","
                   "\"namespace\":\"aimee\",\"tool\":\"exec_command\","
                   "\"arguments\":{\"marker\":0.123456789012345678901234567890}}");
   aimee_block_t call = {0};
   assert(cli_codex_dynamic_call(params, &catalog, &call) == 0);
   assert(!strcmp(call.tool_name, "exec_command") && !strcmp(call.tool_namespace, "aimee"));
   assert(!strcmp(call.tool_id, "call:original"));
   char *exact = cJSON_PrintUnformatted(call.tool_input);
   assert(exact && strstr(exact, "0.123456789012345678901234567890"));
   free(exact);
   aimee_block_free_contents(&call);
   cJSON_ReplaceItemInObject(params, "namespace", cJSON_CreateString("other"));
   assert(cli_codex_dynamic_call(params, &catalog, &call) != 0 && !call.tool_id);
   cJSON_ReplaceItemInObject(params, "namespace", cJSON_CreateString("aimee"));
   cJSON_ReplaceItemInObject(params, "tool", cJSON_CreateString("other"));
   assert(cli_codex_dynamic_call(params, &catalog, &call) != 0);
   cJSON_ReplaceItemInObject(params, "tool", cJSON_CreateString("exec_command"));
   cJSON_AddStringToObject(params, "tool", "exec_command");
   assert(cli_codex_dynamic_call(params, &catalog, &call) != 0);
   cJSON_Delete(params);
   cJSON *content = cJSON_CreateString("host result\nwith stderr");
   aimee_block_t result = {
       .type = AIMEE_BLK_TOOL_RESULT, .tool_id = "call:original", .tool_result = content};
   for (int error = 0; error < 2; error++)
   {
      result.tool_is_error = error;
      cJSON *reply = cli_codex_dynamic_result(&result);
      assert(reply && cJSON_IsTrue(cJSON_GetObjectItem(reply, "success")) == !error);
      const cJSON *text = cJSON_GetArrayItem(cJSON_GetObjectItem(reply, "contentItems"), 0);
      assert(
          !strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(text, "text")), content->valuestring));
      cJSON_Delete(reply);
   }
   result.tool_id = NULL;
   assert(!cli_codex_dynamic_result(&result));
   cJSON_Delete(content);
   cJSON_Delete(child.raw);
   cJSON_Delete(schema);
}

typedef struct
{
   int variant;
   int calls, dispatches;
} dynamic_host_fixture_t;
static int dynamic_host_fixture(const aimee_block_t *call, aimee_block_t *result, void *context)
{
   dynamic_host_fixture_t *state = context;
   state->calls++;
   assert(!strcmp(call->tool_id, "call:original"));
   assert(!strcmp(call->tool_name, "exec_command") && !strcmp(call->tool_namespace, "aimee"));
   char *arguments = cJSON_PrintUnformatted(call->tool_input);
   assert(arguments && strstr(arguments, "0.123456789012345678901234567890"));
   free(arguments);
   if (state->variant == 6 || (state->variant == 5 && state->calls > 1))
      return -1; /* The host source owner, not the SDK call ID, refuses a repeat. */
   state->dispatches++;
   *result = (aimee_block_t){.type = AIMEE_BLK_TOOL_RESULT,
                             .tool_id = strdup(state->variant == 7 ? "wrong-call" : call->tool_id),
                             .tool_result = cJSON_CreateString("host refused\nexact stderr"),
                             .tool_is_error = 1};
   return 0;
}
static void test_dynamic_host_execution(void)
{
   char root[512], script[1024], marker[1024];
   snprintf(root, sizeof root, "%s/aimee-codex-dynamic-XXXXXX", platform_tmpdir());
   assert(mkdtemp(root));
   snprintf(script, sizeof script, "%s/codex", root);
   snprintf(marker, sizeof marker, "%s/reply", root);
   FILE *file = fopen(script, "w");
   assert(file);
   fputs("#!/usr/bin/env python3\nimport "
         "json,os,sys\nmode=int(os.environ['AIMEE_TEST_DYNAMIC_VARIANT'])\ndef send(v): "
         "print(json.dumps(v),flush=True)\nfor line in sys.stdin:\n f=json.loads(line); "
         "m=f.get('method'); p=f.get('params',{})\n if m=='initialize':\n  assert "
         "p['capabilities']['experimentalApi'] is True\n  send({'id':f['id'],'result':{}})\n elif "
         "m=='thread/start':\n  t=p['dynamicTools']; assert len(t)==1 and t[0]['name']=='aimee'\n  "
         "assert t[0]['tools'][0]['name']=='exec_command'\n  "
         "send({'id':f['id'],'result':{'thread':{'id':'thread-owned'}}})\n elif m=='turn/start':\n "
         " assert p['threadId']=='thread-owned'\n  "
         "send({'id':f['id'],'result':{'turn':{'id':'turn-owned'}}})\n  "
         "call='{\"id\":700,\"method\":\"item/tool/"
         "call\",\"params\":{\"threadId\":\"thread-owned\",\"turnId\":\"turn-owned\",\"callId\":"
         "\"call:original\",\"namespace\":\"aimee\",\"tool\":\"exec_command\",\"arguments\":{"
         "\"marker\":0.123456789012345678901234567890}}}'\n  if mode==1: "
         "call=call.replace('\"aimee\"','\"forged\"')\n  if mode==2: "
         "call=call.replace('\"exec_command\"','\"forged\"')\n  if mode==3: "
         "call=call.replace('\"thread-owned\"','\"other-thread\"')\n  if mode==4: "
         "call=call.replace('\"turn-owned\"','\"other-turn\"')\n  if mode==8: "
         "call=call.replace('\"tool\":\"exec_command\"','\"tool\":\"exec_command\",\"tool\":\"exec_"
         "command\"')\n  if mode==13: call+='\\x00{}'.encode().decode('unicode_escape')\n  if "
         "mode==14: call=call.replace('\"aimee\"','\"aimee\\u0000forged\"')\n  if mode==15: "
         "call+=' {}'\n  if mode==17: call=call.replace('\"id\":700','\"id\":700,\"id\":701')\n  "
         "if mode in (8,13,14,15,16,17):\n   with open(os.environ['AIMEE_TEST_DYNAMIC_REPLY'],'w') "
         "as log: log.write('invalid-frame')\n   raw=call.encode()\n   if mode==16: "
         "raw=raw.replace(b'aimee',b'aimee\\xff'.decode('unicode_escape').encode('latin1'))\n   "
         "sys.stdout.buffer.write(raw+b'\\n'.decode('unicode_escape').encode()); "
         "sys.stdout.buffer.flush()\n   sys.stdin.readline()\n   break\n  print(call,flush=True)\n "
         " reply=json.loads(sys.stdin.readline())\n  if mode in (5,9,10,11,12):\n   assert "
         "reply['result']['success'] is False\n   if mode in (5,9): "
         "second=call.replace('\"id\":700','\"id\":701')\n   elif mode==12: "
         "second=call.replace('\"id\":700','\"id\":\"700\"')\n   else: "
         "second=call.replace('\"call:original\"','\"different-call\"')\n   if mode==11: "
         "second=second.replace('\"id\":700','\"id\":7e2')\n   print(second,flush=True)\n   "
         "reply=json.loads(sys.stdin.readline())\n   if mode==12: assert reply['id']=='700'\n   "
         "elif mode in (5,9): assert reply['id']==701\n  with "
         "open(os.environ['AIMEE_TEST_DYNAMIC_REPLY'],'w') as log: log.write(json.dumps(reply))\n  "
         "if mode in (0,9,12):\n   assert "
         "reply['result']=={'contentItems':[{'type':'inputText','text':'host refused\\nexact "
         "stderr'.encode().decode('unicode_escape')}],'success':False}\n   "
         "send({'method':'item/"
         "completed','params':{'item':{'type':'agentMessage','id':'answer','text':'host "
         "complete'}}})\n   "
         "send({'method':'turn/completed','params':{'turn':{'status':'completed'}}})\n  else: "
         "assert reply['error']['code']==-32602\n else: raise AssertionError(m)\n",
         file);
   assert(fclose(file) == 0 && chmod(script, 0700) == 0);
   char *old_path = getenv("PATH") ? strdup(getenv("PATH")) : NULL;
   char path[16384];
   snprintf(path, sizeof path, "%s:%s", root, old_path ? old_path : "");
   assert(!setenv("PATH", path, 1));
   assert(!setenv("AIMEE_TEST_DYNAMIC_REPLY", marker, 1));
   cJSON *schema =
       cJSON_Parse("{\"type\":\"object\",\"properties\":{\"marker\":{\"type\":\"number\"}}}");
   aimee_tool_t child = {.name = "exec_command", .description = "host", .schema = schema};
   aimee_tool_t space = {.name = "aimee",
                         .description = "host",
                         .kind = AIMEE_TOOL_NAMESPACE,
                         .children = &child,
                         .n_children = 1};
   aimee_request_t catalog = {.tools = &space, .n_tools = 1};
   agent_t agent = {0};
   snprintf(agent.name, sizeof agent.name, "dynamic-fixture");
   snprintf(agent.cli_cmd, sizeof agent.cli_cmd, "%s", script);
   agent.cli_idle_timeout_ms = 5000;
   for (int provider = 0; provider < 2; provider++)
      for (int variant = 0; variant < 18; variant++)
      {
         unlink(marker);
         char mode[16];
         snprintf(mode, sizeof mode, "%d", variant);
         assert(!setenv("AIMEE_TEST_DYNAMIC_VARIANT", mode, 1));
         aimee_ir_session_restore_sink(NULL, NULL, 0);
         dynamic_host_fixture_t state = {.variant = variant};
         int success = variant == 0 || variant == 9 || variant == 12;
         int invalid_frame = variant == 8 || variant >= 13;
         int rc;
         if (provider)
         {
            provider_cli_cfg_t cfg = {.agent = &agent,
                                      .cwd = root,
                                      .user_prompt = "current",
                                      .tool_catalog = &catalog,
                                      .tool_call = dynamic_host_fixture,
                                      .tool_context = &state};
            agent_result_t out;
            rc = codex_provider_cli_adapter.execute(&cfg, &out);
            if (invalid_frame)
               assert(strstr(out.error, "invalid-json-rpc-frame"));
            if (success)
               assert(out.success && out.response && !strcmp(out.response, "host complete"));
            free(out.response);
         }
         else
         {
            cli_codex_chat_request_t req = {.cwd = root,
                                            .user_prompt = "current",
                                            .timeout_ms = 5000,
                                            .tool_catalog = &catalog,
                                            .tool_call = dynamic_host_fixture,
                                            .tool_context = &state};
            cli_codex_chat_result_t out;
            codex_stream_capture_t captured = {0};
            rc = cli_codex_chat_stream(&req, codex_capture_cb, &captured, &out);
            if (invalid_frame)
               assert(strstr(out.error, "invalid-json-rpc-frame"));
            if (success && rc)
               fprintf(stderr, "dynamic host error: %s\n", out.error);
            if (success)
               assert(strstr(captured.buf, "host complete"));
         }
         assert(success ? rc == 0 : rc != 0);
         int calls_expected =
             variant == 5 || variant == 9 || variant == 12                                    ? 2
             : variant == 0 || variant == 6 || variant == 7 || variant == 10 || variant == 11 ? 1
                                                                                              : 0;
         int dispatches_expected =
             variant == 9 || variant == 12                                                    ? 2
             : variant == 0 || variant == 5 || variant == 7 || variant == 10 || variant == 11 ? 1
                                                                                              : 0;
         assert(state.calls == calls_expected && state.dispatches == dispatches_expected);
         file = fopen(marker, "r");
         assert(file);
         char reply[4096] = {0};
         assert(fread(reply, 1, sizeof reply - 1, file) && !ferror(file));
         fclose(file);
         if (success)
            assert(strstr(reply, "host refused") && strstr(reply, "false"));
         else if (invalid_frame)
            assert(!strcmp(reply, "invalid-frame") && !state.calls && !state.dispatches);
         else
            assert(strstr(reply, "error") && strstr(reply, "-32602"));
      }
   aimee_ir_session_restore_sink(NULL, NULL, 0);
   cJSON_Delete(schema);
   if (old_path)
      setenv("PATH", old_path, 1);
   else
      unsetenv("PATH");
   free(old_path);
   unsetenv("AIMEE_TEST_DYNAMIC_VARIANT");
   unsetenv("AIMEE_TEST_DYNAMIC_REPLY");
   unlink(marker);
   unlink(script);
   rmdir(root);
}

static int native_probe_host_tool(const aimee_block_t *call, aimee_block_t *result, void *context)
{
   assert(!strcmp(call->tool_id, "call_native_fixture"));
   assert(!strcmp(call->tool_name, "fail") && !strcmp(call->tool_namespace, "fixture"));
   char *arguments = cJSON_PrintUnformatted(call->tool_input);
   assert(arguments && strstr(arguments, "0.123456789012345678901234567890"));
   cJSON *entry = cJSON_CreateObject();
   assert(entry && cJSON_AddStringToObject(entry, "call_id", call->tool_id) &&
          cJSON_AddStringToObject(entry, "name", call->tool_name) &&
          cJSON_AddStringToObject(entry, "namespace", call->tool_namespace) &&
          cJSON_AddStringToObject(entry, "arguments_json", arguments) &&
          cJSON_AddItemToArray(context, entry));
   free(arguments);
   *result = (aimee_block_t){.type = AIMEE_BLK_TOOL_RESULT,
                             .tool_id = strdup(call->tool_id),
                             .tool_result = cJSON_CreateString("refused by host fixture"),
                             .tool_is_error = 1};
   return 0;
}

static int native_history_probe(const char *resume, const char *history_file, int host_tools)
{
   cJSON *arguments = cJSON_Parse("{\"path\":\"fixture.txt\"}");
   cJSON *result = cJSON_CreateString("prior tool result");
   aimee_block_t blocks[] = {
       {.type = AIMEE_BLK_TEXT, .text = "prior user"},
       {.type = AIMEE_BLK_TOOL_USE,
        .tool_id = "call_canonical_fixture",
        .tool_name = "read_file",
        .tool_input = arguments},
       {.type = AIMEE_BLK_TOOL_RESULT, .tool_id = "call_canonical_fixture", .tool_result = result},
       {.type = AIMEE_BLK_TEXT, .text = "prior assistant"}};
   aimee_message_t messages[] = {{.role = "user", .blocks = blocks, .n_blocks = 1},
                                 {.role = "assistant", .blocks = blocks + 1, .n_blocks = 1},
                                 {.role = "user", .blocks = blocks + 2, .n_blocks = 1},
                                 {.role = "assistant", .blocks = blocks + 3, .n_blocks = 1}};
   aimee_request_t history = {.messages = messages, .n_messages = 4};
   aimee_request_t adopted = {0};
   if (history_file)
   {
      FILE *file = fopen(history_file, "rb");
      assert(file);
      char input[65536];
      size_t size = fread(input, 1, sizeof input - 1, file);
      assert(feof(file) && !ferror(file));
      fclose(file);
      cJSON *json = aimee_ir_json_parse_frame(input, size);
      assert(cJSON_IsArray(json));
      adopted.n_messages = cJSON_GetArraySize(json);
      assert(adopted.n_messages > 0 && adopted.n_messages <= 128);
      adopted.messages = calloc((size_t)adopted.n_messages, sizeof *adopted.messages);
      assert(adopted.messages);
      for (int i = 0; i < adopted.n_messages; i++)
         assert(aimee_ir_message_parse(cJSON_GetArrayItem(json, i), adopted.messages + i) == 0);
      cJSON_Delete(json);
   }
   cJSON *host_calls = cJSON_CreateArray();
   cJSON *schema = cJSON_Parse(
       "{\"type\":\"object\",\"properties\":{\"value\":{\"type\":\"integer\"},\"marker\":{\"type\":"
       "\"number\"}},\"required\":[\"value\",\"marker\"],\"additionalProperties\":false}");
   aimee_tool_t child = {.name = "fail", .description = "Local failure fixture", .schema = schema};
   aimee_tool_t space = {.name = "fixture",
                         .description = "Host fixture tools",
                         .kind = AIMEE_TOOL_NAMESPACE,
                         .children = &child,
                         .n_children = 1};
   aimee_request_t catalog = {.tools = &space, .n_tools = 1};
   cli_codex_chat_request_t req = {.thread_id = resume,
                                   .history = history_file ? &adopted
                                              : resume     ? NULL
                                                           : &history,
                                   .user_prompt = resume ? "after restart" : "current user",
                                   .model = "fixture-model",
                                   .system_prompt = "Local native protocol fixture.",
                                   .timeout_ms = 15000};
   req.tool_catalog = host_tools ? &catalog : NULL;
   req.tool_call = host_tools ? native_probe_host_tool : NULL;
   req.tool_context = host_calls;
   cli_codex_chat_result_t out;
   codex_stream_capture_t captured = {0};
   cJSON *events = cJSON_CreateArray();
   aimee_ir_session_set_sink(native_probe_capture, events);
   int rc = cli_codex_chat_stream(&req, codex_capture_cb, &captured, &out);
   aimee_ir_session_set_sink(NULL, NULL);
   cJSON *report = cJSON_CreateObject();
   cJSON_AddItemToObject(report, "events", events);
   cJSON_AddItemToObject(report, "host_calls", host_calls);
   cJSON *persisted = cJSON_CreateArray(), *event;
   cJSON_ArrayForEach(
       event,
       events) if (!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(event, "kind")), "message"))
       assert(cJSON_AddItemToArray(persisted,
                                   cJSON_Duplicate(cJSON_GetObjectItem(event, "payload"), 1)));
   char *history_json = cJSON_PrintUnformatted(persisted);
   assert(history_json && cJSON_AddStringToObject(report, "history_json", history_json));
   free(history_json);
   cJSON_Delete(persisted);
   cJSON_Delete(schema);
   cJSON_AddStringToObject(report, "thread_id", out.thread_id);
   cJSON_AddStringToObject(report, "text", captured.buf);
   cJSON_AddStringToObject(report, "error", out.error);
   char *raw = cJSON_PrintUnformatted(report);
   if (raw)
      puts(raw);
   free(raw);
   cJSON_Delete(report);
   cJSON_Delete(arguments);
   cJSON_Delete(result);
   aimee_request_free(&adopted);
   return rc ? 1 : 0;
}

static void test_execute_refuses_missing_configuration(void)
{
   agent_result_t out;
   assert(agent_execute_cli_codex(NULL, "system", "user", &out) == -1);
   assert(strcmp(out.error, "missing agent") == 0);
   assert(agent_execute_cli_codex(NULL, "system", "user", NULL) == -1);
}

int main(int argc, char **argv)
{
   test_execute_refuses_missing_configuration();
   test_execute_rejects_missing_inputs();
   signal(SIGPIPE, SIG_IGN);
   if (argc >= 2 && !strcmp(argv[1], "--native-history-probe"))
   {
      const char *resume = NULL, *history = NULL;
      int host_tools = 0;
      for (int i = 2; i < argc; i++)
      {
         if (!strcmp(argv[i], "--host-tools"))
            host_tools = 1;
         else if (!strcmp(argv[i], "--import") && !history && i + 1 < argc)
            history = argv[++i];
         else if (argv[i][0] != '-' && !resume)
            resume = argv[i];
         else
            return 2;
      }
      return native_history_probe(resume, history, host_tools);
   }
   test_dynamic_tool_boundaries();
   test_dynamic_host_execution();
   test_canonical_history_adoption();
   puts("canonical history: typed adoption, resume protocol and refusal before model turn "
        "passed");

   printf("test_parse_agent_message_extracts_text... ");
   test_parse_agent_message_extracts_text();
   printf("OK\n");

   printf("test_parse_agent_message_skips_other_item_types... ");
   test_parse_agent_message_skips_other_item_types();
   printf("OK\n");

   printf("test_parse_agent_message_skips_other_methods... ");
   test_parse_agent_message_skips_other_methods();
   printf("OK\n");

   printf("test_parse_agent_message_handles_invalid_json... ");
   test_parse_agent_message_handles_invalid_json();
   printf("OK\n");

   printf("test_parse_agent_message_empty_text_is_skipped... ");
   test_parse_agent_message_empty_text_is_skipped();
   printf("OK\n");

   printf("test_parse_token_usage_extracts_input_output... ");
   test_parse_token_usage_extracts_input_output();
   printf("OK\n");

   printf("test_parse_token_usage_missing_fields_default_zero... ");
   test_parse_token_usage_missing_fields_default_zero();
   printf("OK\n");

   printf("test_parse_token_usage_skips_other_methods... ");
   test_parse_token_usage_skips_other_methods();
   printf("OK\n");

   printf("test_parse_token_usage_null_outparams_ok... ");
   test_parse_token_usage_null_outparams_ok();
   printf("OK\n");

   printf("test_parse_error_notification_unwraps_nested_message... ");
   test_parse_error_notification_unwraps_nested_message();
   printf("OK\n");

   printf("test_parse_turn_completed_error... ");
   test_parse_turn_completed_error();
   printf("OK\n");

   printf("test_parse_error_message_skips_success... ");
   test_parse_error_message_skips_success();
   printf("OK\n");

   printf("test_parse_method_extracts_string... ");
   test_parse_method_extracts_string();
   printf("OK\n");

   printf("test_parse_method_response_envelope_has_no_method... ");
   test_parse_method_response_envelope_has_no_method();
   printf("OK\n");

   printf("test_parse_method_truncates_long_names... ");
   test_parse_method_truncates_long_names();
   printf("OK\n");

   printf("test_parse_method_invalid_inputs... ");
   test_parse_method_invalid_inputs();
   printf("OK\n");

   printf("test_parse_server_request_method_extracts_approval... ");
   test_parse_server_request_method_extracts_approval();
   printf("OK\n");

   printf("test_parse_server_request_method_extracts_legacy_approval... ");
   test_parse_server_request_method_extracts_legacy_approval();
   printf("OK\n");

   printf("test_parse_server_request_method_skips_notifications_and_responses... ");
   test_parse_server_request_method_skips_notifications_and_responses();
   printf("OK\n");

   printf("test_parse_server_request_method_invalid_inputs... ");
   test_parse_server_request_method_invalid_inputs();
   printf("OK\n");

   printf("test_approval_decision_respects_autonomous_mode... ");
   test_approval_decision_respects_autonomous_mode();
   printf("OK\n");

   printf("test_approval_decision_uses_legacy_review_values... ");
   test_approval_decision_uses_legacy_review_values();
   printf("OK\n");

   printf("test_approval_decision_skips_unknown_methods... ");
   test_approval_decision_skips_unknown_methods();
   printf("OK\n");

   printf("test_server_restart_command_detection_blocks_lifecycle_writes... ");
   test_server_restart_command_detection_blocks_lifecycle_writes();
   printf("OK\n");

   printf("test_server_restart_command_detection_allows_read_only_checks... ");
   test_server_restart_command_detection_allows_read_only_checks();
   printf("OK\n");

   printf("test_spawn_failure_captures_stderr_and_exit_status... ");
   test_spawn_failure_captures_stderr_and_exit_status();
   printf("OK\n");

   printf("test_parse_tool_action_counts_non_agent_items... ");
   test_parse_tool_action_counts_non_agent_items();
   printf("OK\n");

   printf("test_parse_tool_action_skips_agent_message... ");
   test_parse_tool_action_skips_agent_message();
   printf("OK\n");

   printf("test_parse_tool_action_skips_other_methods... ");
   test_parse_tool_action_skips_other_methods();
   printf("OK\n");

   printf("test_agent_execute_cli_codex_uses_requested_cwd... ");
   test_agent_execute_cli_codex_uses_requested_cwd();
   printf("OK\n");

   printf("test_chat_stream_splits_distinct_agent_message_items... ");
   test_chat_stream_splits_distinct_agent_message_items();
   printf("OK\n");

   printf("test_chat_stream_splits_completed_delta_items_without_ids... ");
   test_chat_stream_splits_completed_delta_items_without_ids();
   printf("OK\n");

   printf("test_chat_stream_replaces_invalid_utf8_prompt_bytes... ");
   test_chat_stream_replaces_invalid_utf8_prompt_bytes();
   printf("OK\n");

   test_operator_replies_reach_native_child();
   printf("All cli_codex parse tests passed.\n");
   return 0;
}
