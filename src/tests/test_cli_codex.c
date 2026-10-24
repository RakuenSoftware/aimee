/* Codex JSON-RPC parsing and child protocol boundary fixtures. */
#include <assert.h>
#include <fcntl.h>
#include <sys/mman.h>
#include "aimee_sha256.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
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
   agent_result_t out = {0};
   agent_t agent = {0};
   assert(codex_provider_cli_adapter.execute(
              &(provider_cli_cfg_t){
                  .agent = &agent, .cwd = NULL, .system_prompt = NULL, .user_prompt = "prompt"},
              NULL) == -1);
   assert(codex_provider_cli_adapter.execute(
              &(provider_cli_cfg_t){
                  .agent = NULL, .cwd = NULL, .system_prompt = NULL, .user_prompt = "prompt"},
              &out) == -1);
   assert(!out.success && !out.canonical_response.n_content && !out.response);
   assert(codex_provider_cli_adapter.execute(NULL, &out) == -1);
   provider_cli_cfg_t cfg = {.agent = &agent, .user_prompt = "prompt"};
   assert(codex_provider_cli_adapter.execute(&cfg, NULL) == -1);
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

/* Run the real adapter through owned interpreter and directory descriptors.
 * Protocol scripts answer config/read themselves: profile verification, ready,
 * thread adoption and all byte-frame checks remain production code. */
extern char **environ;
typedef struct
{
   const provider_cli_cfg_t *cfg;
   int ready;
   struct stat executable, directory;
} owned_fixture_t;
static int owned_guard_child(void *context, int executable, int cwd)
{
   owned_fixture_t *state = context;
   struct stat program, directory;
   assert(executable >= 0 && cwd >= 0 && executable != cwd);
   assert(!fstat(executable, &program) && !fstat(cwd, &directory));
   assert(program.st_dev == state->executable.st_dev && program.st_ino == state->executable.st_ino);
   assert(directory.st_dev == state->directory.st_dev &&
          directory.st_ino == state->directory.st_ino);
   return 0;
}
static int owned_guard_parent(void *context, pid_t child)
{
   assert(context && child > 0);
   return 0;
}
static int owned_ready(const char *digest, cli_codex_host_replay_t *replay, void *context)
{
   owned_fixture_t *state = context;
   assert(strlen(digest) == 64 && !state->ready++);
   replay->history = state->cfg->history;
   replay->resume_id = state->cfg->resume_id;
   return 0;
}
static int owned_execute(const provider_cli_cfg_t *cfg, agent_result_t *out)
{
   if (cfg->codex_host)
   {
      server_control_scope_t *scope =
          server_control_scope_open("probe-session", "probe-turn", "fixture");
      assert(scope);
      server_control_scope_t *previous = server_control_scope_bind(scope);
      int rc = codex_provider_cli_adapter.execute(cfg, out);
      server_control_scope_bind(previous);
      server_control_scope_close(scope);
      return rc;
   }
   assert(cfg->cwd && cfg->cwd[0]);
   char script[1024], entry[1024];
   assert(snprintf(script, sizeof script, "%s", cfg->agent->cli_cmd[0] ? cfg->agent->cli_cmd : "") <
          (int)sizeof script);
   if (!script[0])
      assert(snprintf(script, sizeof script, "%s/codex", cfg->cwd) < (int)sizeof script);
   assert(snprintf(entry, sizeof entry, "%s/app-server", cfg->cwd) < (int)sizeof entry);
   FILE *file = fopen(script, "r");
   assert(file);
   char shebang[128];
   assert(fgets(shebang, sizeof shebang, file) && !fclose(file));
   const char *interpreter = strstr(shebang, "python3") ? "/usr/bin/python3" : "/bin/sh";
   assert(!symlink(script, entry));
   cJSON *config = cJSON_CreateObject();
   cJSON *providers = cJSON_AddObjectToObject(config, "model_providers");
   cJSON *provider = cJSON_AddObjectToObject(providers, "aimee_ir");
   cJSON *headers = cJSON_AddObjectToObject(provider, "http_headers");
   assert(cJSON_AddStringToObject(provider, "base_url", "http://127.0.0.1:1/v1"));
   assert(cJSON_AddStringToObject(headers, "Authorization", "Bearer fixture-token"));
   cJSON *projects = cJSON_AddObjectToObject(config, "projects");
   assert(cJSON_AddObjectToObject(projects, cfg->cwd));
   char *raw = cJSON_PrintUnformatted(config);
   assert(raw);
   size_t count = 0;
   for (char **e = environ; *e; e++)
      if (!strncmp(*e, "AIMEE_TEST_", 11) && strncmp(*e, "AIMEE_TEST_OWNED_PROFILE=", 25))
         count++;
   char **environment = calloc(count + 5, sizeof *environment);
   assert(environment);
   size_t at = 0;
   for (char **e = environ; *e; e++)
      if (!strncmp(*e, "AIMEE_TEST_", 11) && strncmp(*e, "AIMEE_TEST_OWNED_PROFILE=", 25))
         environment[at++] = strdup(*e);
   environment[at++] = strdup("PATH=/usr/bin:/bin");
   environment[at] = malloc(strlen(raw) + sizeof "AIMEE_TEST_OWNED_PROFILE=");
   assert(environment[at]);
   sprintf(environment[at++], "AIMEE_TEST_OWNED_PROFILE=%s", raw);
   free(raw);
   owned_fixture_t state = {.cfg = cfg};
   cli_codex_host_launch_t host = {
       .executable_fd = open(interpreter, O_RDONLY | O_CLOEXEC),
       .cwd_fd = open(cfg->cwd, O_RDONLY | O_DIRECTORY | O_CLOEXEC),
       .home = cfg->cwd,
       .cwd = cfg->cwd,
       .build_digest = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
       .gateway_url = "http://127.0.0.1:1",
       .gateway_token = "fixture-token",
       .environment = environment,
       .expected_config = config,
       .ready = owned_ready,
       .context = &state};
   assert(host.executable_fd >= 0 && host.cwd_fd >= 0);
   assert(!fstat(host.executable_fd, &state.executable) && !fstat(host.cwd_fd, &state.directory));
   cli_native_spawn_guard_t guard = {owned_guard_child, owned_guard_parent, &state};
   host.spawn_guard = &guard;
   provider_cli_cfg_t owned = *cfg;
   owned.codex_host = &host;
   server_control_scope_t *scope = NULL, *previous = server_control_scope_current();
   if (!previous)
   {
      scope = server_control_scope_open("owned-session", "owned-turn", "fixture");
      assert(scope);
      server_control_scope_bind(scope);
   }
   int rc = codex_provider_cli_adapter.execute(&owned, out);
   if (scope)
   {
      server_control_scope_bind(previous);
      server_control_scope_close(scope);
   }
   close(host.executable_fd);
   close(host.cwd_fd);
   assert(!unlink(entry));
   for (size_t i = 0; environment[i]; i++)
      free(environment[i]);
   free(environment);
   cJSON_Delete(config);
   return rc;
}

static void test_execute_requires_owned_resources(void)
{
   char root[256], script[512], marker[512], entry[512];
   assert(snprintf(root, sizeof root, "%s/aimee-codex-unowned-XXXXXX", platform_tmpdir()) <
          (int)sizeof root);
   assert(mkdtemp(root));
   snprintf(script, sizeof script, "%s/unowned", root);
   snprintf(entry, sizeof entry, "%s/app-server", root);
   snprintf(marker, sizeof marker, "%s/started", root);
   FILE *file = fopen(script, "w");
   assert(file);
   fprintf(file, "#!/bin/sh\nprintf started > '%s'\nexit 42\n", marker);
   assert(!fclose(file) && !chmod(script, 0700) && !symlink(script, entry));
   agent_t agent = {0};
   snprintf(agent.cli_cmd, sizeof agent.cli_cmd, "%s", script);
   agent.cli_idle_timeout_ms = 1000;
   provider_cli_cfg_t cfg = {
       .agent = &agent, .cwd = root, .user_prompt = "owned resources required"};
   agent_result_t out = {0};
   assert(codex_provider_cli_adapter.execute(&cfg, &out) != 0);
   assert(access(marker, F_OK) != 0); /* Actual predecessor starts the ambient executable. */
   cJSON *expected = cJSON_CreateObject();
   char *environment[] = {"PATH=/usr/bin:/bin", NULL};
   cli_codex_host_launch_t host = {
       .executable_fd = open("/bin/sh", O_RDONLY | O_CLOEXEC),
       .cwd_fd = open(root, O_RDONLY | O_DIRECTORY | O_CLOEXEC),
       .home = root,
       .cwd = root,
       .build_digest = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
       .gateway_url = "http://127.0.0.1:1",
       .gateway_token = "fixture-token",
       .environment = environment,
       .expected_config = expected,
       .ready = owned_ready};
   assert(host.executable_fd >= 0 && host.cwd_fd >= 0);
   cfg.codex_host = &host;
   assert(!server_control_scope_current());
   assert(codex_provider_cli_adapter.execute(&cfg, &out) != 0);
   assert(strstr(out.error, "control scope") && access(marker, F_OK) != 0);
   server_control_scope_t *scope =
       server_control_scope_open("negative-session", "negative-turn", "fixture");
   assert(scope);
   server_control_scope_t *previous = server_control_scope_bind(scope);
   int fd = host.executable_fd;
   host.executable_fd = -1;
   assert(codex_provider_cli_adapter.execute(&cfg, &out) != 0);
   assert(access(marker, F_OK) != 0);
   int closed_fd = dup(fd);
   assert(closed_fd >= 0 && !close(closed_fd));
   host.executable_fd = closed_fd;
   assert(codex_provider_cli_adapter.execute(&cfg, &out) != 0);
   assert(access(marker, F_OK) != 0);
   host.executable_fd = fd;
   host.ready = NULL;
   assert(codex_provider_cli_adapter.execute(&cfg, &out) != 0);
   assert(access(marker, F_OK) != 0);
   server_control_scope_bind(previous);
   server_control_scope_close(scope);
   close(host.executable_fd);
   close(host.cwd_fd);
   cJSON_Delete(expected);
   assert(!unlink(entry) && !unlink(script) && !rmdir(root));
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

   agent_result_t out = {0};
   int rc = owned_execute(&(provider_cli_cfg_t){.agent = &agent,
                                                .cwd = platform_tmpdir(),
                                                .system_prompt = NULL,
                                                .user_prompt = "hello"},
                          &out);
   assert(rc == -1);
   assert(strstr(out.error, "status 42") != NULL);
   assert(strstr(out.error, "fixture-args:") != NULL);
   assert(strstr(out.error, "fixture-stderr-message") != NULL);

   agent.write_capable = 1;
   rc = owned_execute(&(provider_cli_cfg_t){.agent = &agent,
                                            .cwd = platform_tmpdir(),
                                            .system_prompt = NULL,
                                            .user_prompt = "hello"},
                      &out);
   assert(rc == -1);
   assert(strstr(out.error, "fixture-args:") != NULL);
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

/* Test-only request convenience: all vectors exercise the shipping canonical
 * adapter, not the retired second callback transport. */
typedef struct
{
   const agent_network_t *network;
   const char *thread_id, *home, *session_id, *cwd, *system_prompt, *user_prompt;
   const cli_codex_host_launch_t *host_launch;
   const aimee_request_t *history, *tool_catalog;
   int (*tool_call)(const aimee_block_t *, aimee_block_t *, void *);
   void *tool_context;
   const char *model, *reasoning_effort;
   int timeout_ms, autonomous;
} adapter_fixture_request_t;
typedef struct
{
   char thread_id[128], error[512];
} adapter_fixture_result_t;
typedef struct
{
   int (*callback)(const char *, const char *, void *);
   void *context;
   aimee_ir_session_sink_fn previous;
   void *previous_context;
   char last_message_id[1024];
   size_t observed_text;
} adapter_fixture_capture_t;
static int adapter_fixture_event(const aimee_session_event_t *event, void *context)
{
   adapter_fixture_capture_t *c = context;
   if (c->previous && c->previous(event, c->previous_context))
      return -1;
   if (c->callback && event->kind == AIMEE_SESSION_MESSAGE && !event->message_partial &&
       event->message && !strcmp(event->message->role, "assistant"))
   {
      if (strcmp(c->last_message_id, event->message_id))
      {
         snprintf(c->last_message_id, sizeof c->last_message_id, "%s", event->message_id);
         c->observed_text = 0;
         if (c->callback("turn_start", NULL, c->context))
            return -1;
      }
      size_t offset = 0;
      for (int i = 0; i < event->message->n_blocks; ++i)
      {
         const aimee_block_t *block = event->message->blocks + i;
         if (block->type != AIMEE_BLK_TEXT || !block->text)
            continue;
         size_t length = strlen(block->text);
         if (offset + length > c->observed_text &&
             c->callback("text",
                         block->text + (c->observed_text > offset ? c->observed_text - offset : 0),
                         c->context))
            return -1;
         offset += length;
      }
      c->observed_text = offset;
   }
   return 0;
}
static int adapter_fixture_bind(const char *id, void *context)
{
   adapter_fixture_result_t *result = context;
   snprintf(result->thread_id, sizeof result->thread_id, "%s", id);
   return 0;
}
static int execute_adapter_fixture(const adapter_fixture_request_t *request,
                                   int (*callback)(const char *, const char *, void *),
                                   void *context, adapter_fixture_result_t *result)
{
   agent_t agent = {0};
   snprintf(agent.model, sizeof agent.model, "%s", request->model ? request->model : "");
   agent.autonomous = request->autonomous;
   agent.cli_idle_timeout_ms = request->timeout_ms;
   provider_cli_cfg_t cfg = {.agent = &agent,
                             .network = request->network,
                             .cwd = request->cwd,
                             .home = request->home,
                             .system_prompt = request->system_prompt,
                             .user_prompt = request->user_prompt,
                             .history = request->history,
                             .resume_id = request->thread_id,
                             .codex_host = request->host_launch,
                             .session_id = request->session_id,
                             .reasoning_effort = request->reasoning_effort,
                             .tool_catalog = request->tool_catalog,
                             .tool_call = request->tool_call,
                             .tool_context = request->tool_context,
                             .bind_session = adapter_fixture_bind,
                             .bind_context = result};
   memset(result, 0, sizeof *result);
   if (request->thread_id)
      snprintf(result->thread_id, sizeof result->thread_id, "%s", request->thread_id);
   adapter_fixture_capture_t capture = {.callback = callback, .context = context};
   capture.previous = aimee_ir_session_get_sink(&capture.previous_context);
   aimee_ir_session_set_sink(adapter_fixture_event, &capture);
   agent_result_t actual = {0};
   int rc = owned_execute(&cfg, &actual);
   snprintf(result->error, sizeof result->error, "%s", actual.error);
   if (!rc && callback)
      rc = callback("turn_end", NULL, context);
   int failed = aimee_ir_session_capture_failed();
   aimee_ir_session_restore_sink(capture.previous, capture.previous_context, failed);
   agent_result_clear_response(&actual);
   return rc;
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
   fputs("#!/usr/bin/env python3\nimport json, os, sys\ndef profile(f):\n "
         "c=json.loads(os.environ['AIMEE_TEST_OWNED_PROFILE'])\n assert os.getcwd() in "
         "c['projects']\n return "
         "{'id':f['id'],'result':{'config':c,'layers':[{'name':{'type':'user','file':os.getcwd()+'/"
         ".codex/config.toml'},'config':c}]}}\ndef send(value):\n print(json.dumps(value), "
         "flush=True)\nfor i in range(4):\n frame=json.loads(sys.stdin.readline())\n if "
         "frame.get('method')=='config/read':\n  send(profile(frame))\n  continue\n if "
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
   adapter_fixture_request_t req = {.cwd = root, .user_prompt = "hello", .timeout_ms = 5000};
   adapter_fixture_result_t out;
   codex_stream_capture_t cap = {0};
   assert(execute_adapter_fixture(&req, codex_capture_cb, &cap, &out) == 0);
   assert(!strstr(cap.buf, "Operator replies received")); /* Host model output is not duplicated. */
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

static int stdio_hole_case;
static void test_canonical_history_adoption(void)
{
   char root[256], script[512], marker[512];
   snprintf(root, sizeof root, "%s/aimee-codex-adoption-XXXXXX", platform_tmpdir());
   assert(mkdtemp(root));
   snprintf(script, sizeof script, "%s/codex", root);
   snprintf(marker, sizeof marker, "%s/requests", root);
   FILE *fp = fopen(script, "w");
   assert(fp);
   fputs("#!/usr/bin/env python3\nimport json, os, sys, stat\nassert "
         "all(stat.S_ISFIFO(os.fstat(i).st_mode) for i in (0,1,2))\ndef profile(f):\n "
         "c=json.loads(os.environ['AIMEE_TEST_OWNED_PROFILE'])\n if "
         "os.environ.get('AIMEE_TEST_BAD_PROFILE')=='1': c['extra_mutation']=True\n assert "
         "os.getcwd() in "
         "c['projects']\n return "
         "{'id':f['id'],'result':{'config':c,'layers':[{'name':{'type':'user','file':os.getcwd()+'/"
         ".codex/config.toml'},'config':c}]}}\n"
         "def send(v):\n print(json.dumps(v),flush=True)\n"
         "adopted=False\nlast=0\n"
         "for line in sys.stdin:\n f=json.loads(line)\n m=f.get('method')\n"
         " if m=='initialized': continue\n"
         " with open(os.environ['AIMEE_TEST_HISTORY_LOG'],'a') as log: "
         "log.write(json.dumps(f)+'\\n')\n"
         " assert f['id']>last\n last=f['id']\n p=f.get('params',{})\n"
         " if m=='initialize': send({'id':f['id'],'result':{}})\n"
         " elif m=='config/read': send(profile(f))\n"
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
   for (int resume = 0; resume < (stdio_hole_case ? 1 : 2); resume++)
      for (int refuse = 0; refuse < (stdio_hole_case ? 1 : 4); refuse++)
      {
         unlink(marker);
         assert(!setenv("AIMEE_TEST_BAD_PROFILE", refuse == 3 ? "1" : "0", 1));
         setenv("AIMEE_TEST_HISTORY_SESSION", resume ? "thread/resume" : "thread/start", 1);
         setenv("AIMEE_TEST_HISTORY_REFUSE", refuse == 1 ? "1" : "0", 1);
         /* Invalid canonical content must refuse before injection/turn/start. */
         blocks[0].type = refuse == 2 ? AIMEE_BLK_UNKNOWN : AIMEE_BLK_TEXT;
         int rc;

         provider_cli_cfg_t cfg = {.agent = &agent,
                                   .cwd = root,
                                   .history = &history,
                                   .user_prompt = "current user",
                                   .resume_id = resume ? "t-history" : NULL};
         agent_result_t out = {0};
         rc = owned_execute(&cfg, &out);
         if (!refuse)
            assert(out.success && !out.response && !out.canonical_response.n_content);
         else if (refuse == 3)
            assert(strstr(out.error, "native SDK configuration could not be verified"));
         else
            assert(strstr(out.error, "canonical history adoption failed"));
         agent_result_clear_response(&out);

         assert(refuse ? rc != 0 : rc == 0);
         fp = fopen(marker, "r");
         assert(fp);
         char log[16384] = {0};
         size_t n = fread(log, 1, sizeof log - 1, fp);
         assert(n && !ferror(fp));
         fclose(fp);
         assert((strstr(log, "thread/inject_items") != NULL) == (refuse != 2 && refuse != 3));
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
   unsetenv("AIMEE_TEST_BAD_PROFILE");
   unsetenv("AIMEE_TEST_HISTORY_LOG");
   unsetenv("AIMEE_TEST_HISTORY_SESSION");
   unsetenv("AIMEE_TEST_HISTORY_REFUSE");
   unlink(marker);
   unlink(script);
   rmdir(root);
}

static void test_owned_stdio_holes(void)
{
   fflush(NULL);
   for (int holes = 1; holes < 8; holes++)
   {
      pid_t child = fork();
      assert(child >= 0);
      if (!child)
      {
         for (int fd = 0; fd < 3; fd++)
            if (holes & (1 << fd))
               close(fd);
         stdio_hole_case = 1;
         test_canonical_history_adoption();
         _exit(0);
      }
      int status;
      assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && !WEXITSTATUS(status));
   }
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
         "json,os,sys,signal\ndef profile(f):\n "
         "c=json.loads(os.environ['AIMEE_TEST_OWNED_PROFILE'])\n assert os.getcwd() in "
         "c['projects']\n return "
         "{'id':f['id'],'result':{'config':c,'layers':[{'name':{'type':'user','file':os.getcwd()+'/"
         ".codex/"
         "config.toml'},'config':c}]}}\nsignal.signal(signal.SIGTERM,signal.SIG_IGN)\nmode=int(os."
         "environ['"
         "AIMEE_TEST_DYNAMIC_VARIANT'])\ndef send(v): "
         "print(json.dumps(v),flush=True)\nfor line in sys.stdin:\n f=json.loads(line); "
         "m=f.get('method'); p=f.get('params',{})\n if m=='initialize':\n  assert "
         "p['capabilities']['experimentalApi'] is True\n  send({'id':f['id'],'result':{}})\n elif "
         "m=='config/read': send(profile(f))\n elif m=='thread/start':\n  t=p['dynamicTools']; "
         "assert len(t)==1 and t[0]['name']=='aimee'\n  "
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

      provider_cli_cfg_t cfg = {.agent = &agent,
                                .cwd = root,
                                .user_prompt = "current",
                                .tool_catalog = &catalog,
                                .tool_call = dynamic_host_fixture,
                                .tool_context = &state};
      agent_result_t out = {0};
      rc = owned_execute(&cfg, &out);
      if (invalid_frame)
         assert(strstr(out.error, "invalid-json-rpc-frame"));
      if (success)
         assert(out.success && !out.response && !out.canonical_response.n_content);
      agent_result_clear_response(&out);

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
   assert(!strcmp(call->tool_name, "fail"));
   int flat = getenv("AIMEE_TEST_FLAT_HOST_TOOLS") != NULL;
   assert(flat ? !call->tool_namespace
               : call->tool_namespace && !strcmp(call->tool_namespace, "fixture"));
   char *arguments = cJSON_PrintUnformatted(call->tool_input);
   assert(arguments && strstr(arguments, "0.123456789012345678901234567890"));
   cJSON *entry = cJSON_CreateObject();
   assert(entry && cJSON_AddStringToObject(entry, "call_id", call->tool_id) &&
          cJSON_AddStringToObject(entry, "name", call->tool_name) &&
          (call->tool_namespace ? cJSON_AddStringToObject(entry, "namespace", call->tool_namespace)
                                : cJSON_AddNullToObject(entry, "namespace")) &&
          cJSON_AddStringToObject(entry, "arguments_json", arguments) &&
          cJSON_AddItemToArray(context, entry));
   free(arguments);
   *result = (aimee_block_t){.type = AIMEE_BLK_TOOL_RESULT,
                             .tool_id = strdup(call->tool_id),
                             .tool_result = cJSON_CreateString("refused by host fixture"),
                             .tool_is_error = 1};
   return 0;
}

static char probe_launch_digest[65];
static int native_probe_ready(const char *digest, cli_codex_host_replay_t *replay, void *context)
{
   (void)replay;
   assert(context == probe_launch_digest && !probe_launch_digest[0] && strlen(digest) == 64);
   snprintf(probe_launch_digest, sizeof probe_launch_digest, "%s", digest);
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
   aimee_request_t catalog = {.tools = getenv("AIMEE_TEST_FLAT_HOST_TOOLS") ? &child : &space,
                              .n_tools = 1};
   adapter_fixture_request_t req = {.thread_id = resume,
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
   cli_codex_host_launch_t launch = {0};
   cJSON *expected = NULL;
   char home[4200], codex[4200], temporary[4200], directory[4200], build[65];
   char *environment[] = {home,
                          codex,
                          temporary,
                          "PATH=/usr/bin:/bin",
                          "LANG=C.UTF-8",
                          "OTEL_SDK_DISABLED=true",
                          "HTTP_PROXY=http://127.0.0.1:1",
                          "HTTPS_PROXY=http://127.0.0.1:1",
                          "NO_PROXY=127.0.0.1,localhost",
                          NULL};
   const char *configuration = getenv("AIMEE_TEST_OWNED_CODEX_CONFIG");
   if (configuration)
   {
      FILE *file = fopen(configuration, "r");
      assert(file);
      char bytes[65536];
      size_t length = fread(bytes, 1, sizeof bytes, file);
      assert(!ferror(file) && feof(file) && !fclose(file));
      expected = aimee_ir_json_parse_frame(bytes, length);
      assert(expected && host_tools);
      const char *root = getenv("HOME"), *binary = getenv("AIMEE_TEST_OWNED_CODEX_BINARY");
      assert(root && binary);
      assert(snprintf(home, sizeof home, "HOME=%s", root) < (int)sizeof home);
      assert(snprintf(codex, sizeof codex, "CODEX_HOME=%s/.codex", root) < (int)sizeof codex);
      assert(snprintf(temporary, sizeof temporary, "TMPDIR=%s", root) < (int)sizeof temporary);
      assert(snprintf(directory, sizeof directory, "%s/workspace", root) < (int)sizeof directory);
      launch =
          (cli_codex_host_launch_t){.executable_fd = open(binary, O_RDONLY | O_CLOEXEC),
                                    .cwd_fd = open(directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC),
                                    .home = root,
                                    .cwd = directory,
                                    .build_digest = build,
                                    .gateway_url = getenv("AIMEE_TEST_OWNED_CODEX_URL"),
                                    .gateway_token = "fixture-token",
                                    .environment = environment,
                                    .expected_config = expected,
                                    .ready = native_probe_ready,
                                    .context = probe_launch_digest};
      assert(launch.executable_fd >= 0 && launch.cwd_fd >= 0 && launch.gateway_url);
      struct stat binary_status;
      assert(!fstat(launch.executable_fd, &binary_status) && binary_status.st_size > 0);
      void *image = mmap(NULL, (size_t)binary_status.st_size, PROT_READ, MAP_PRIVATE,
                         launch.executable_fd, 0);
      assert(image != MAP_FAILED && !aimee_sha256_hex(image, (size_t)binary_status.st_size, build));
      assert(!munmap(image, (size_t)binary_status.st_size));
      req.host_launch = &launch;
   }
   adapter_fixture_result_t out;
   codex_stream_capture_t captured = {0};
   cJSON *events = cJSON_CreateArray();
   aimee_ir_session_set_sink(native_probe_capture, events);
   int rc = execute_adapter_fixture(&req, codex_capture_cb, &captured, &out);
   aimee_ir_session_set_sink(NULL, NULL);
   cJSON *report = cJSON_CreateObject();
   cJSON_AddStringToObject(report, "host_launch_digest", probe_launch_digest);
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
   if (configuration)
   {
      close(launch.executable_fd);
      close(launch.cwd_fd);
      cJSON_Delete(expected);
   }
   return rc ? 1 : 0;
}

static void test_execute_refuses_missing_configuration(void)
{
   agent_result_t out = {0};
   assert(codex_provider_cli_adapter.execute(
              &(provider_cli_cfg_t){
                  .agent = NULL, .cwd = NULL, .system_prompt = "system", .user_prompt = "user"},
              &out) == -1);
   assert(!out.success && !out.canonical_response.n_content && !out.response);
   assert(codex_provider_cli_adapter.execute(
              &(provider_cli_cfg_t){
                  .agent = NULL, .cwd = NULL, .system_prompt = "system", .user_prompt = "user"},
              NULL) == -1);
}

int main(int argc, char **argv)
{
   test_execute_refuses_missing_configuration();
   test_execute_rejects_missing_inputs();
   test_execute_requires_owned_resources();
   if (argc == 2 && !strcmp(argv[1], "--ownership-negative-only"))
      return 0;
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

   test_owned_stdio_holes();
   test_operator_replies_reach_native_child();
   printf("All cli_codex parse tests passed.\n");
   return 0;
}
