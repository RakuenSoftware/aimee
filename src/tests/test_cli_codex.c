/* test_cli_codex.c: pure-parse tests for the codex app-server JSON-RPC
 * helpers in cli_codex.c. No subprocess; we feed synthetic frames
 * captured from a real `codex app-server` session and assert the
 * extracted bits. */
#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "cli_codex.h"

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
   char path[] = "/tmp/aimee-codex-fixture-XXXXXX";
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
   assert(strstr(out.error, "approval_policy=\"untrusted\"") != NULL);
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

static void test_agent_execute_cli_codex_uses_requested_cwd(void)
{
   char root[] = "/tmp/aimee-codex-cwd-XXXXXX";
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
         "i=0\n"
         "while IFS= read -r line; do\n"
         "  i=$((i + 1))\n"
         "  case \"$i\" in\n"
         "    1) printf '%s\\n' '{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{}}' ;;\n"
         "    2) printf '%s\\n' "
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
   char root[] = "/tmp/aimee-codex-stream-XXXXXX";
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
   char root[] = "/tmp/aimee-codex-stream-noids-XXXXXX";
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
   char root[] = "/tmp/aimee-codex-utf8-XXXXXX";
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

int main(void)
{
   signal(SIGPIPE, SIG_IGN);

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

   printf("All cli_codex parse tests passed.\n");
   return 0;
}
