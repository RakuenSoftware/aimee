#include "db1_client/agent_jobs.h"
#include "aimee.h"
#include <aimee/tools/agent_tools.h>
#include "cJSON.h"
#include "agent_config.h"
#include "agent_source_authority.h"
#include "request_context.h"
#include "server_session_capture.h"
#include "script_tool_execution.h"
#include "script_rpc.h"
#include <arpa/inet.h>
#include <sys/ioctl.h>
#include <signal.h>
#include <time.h>
#include "modules/workspace/workspace_provider.h"
#include "log.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_dispatch_calls = 0;
static int g_dispatch_error;
static int g_scope_capture_refused;
static int g_source_capture_refused;
static int g_dispatch_blocked;
static const char *g_dispatch_result;
static atomic_int dispatch_entered, dispatch_released;
static _Thread_local atomic_int *request_cancel;
static _Thread_local const agent_request_cancel_scope_t *request_cancel_scope;

char *server_script_tool_execute(const cJSON *frame, const char *attempt, int *is_error)
{
   (void)attempt;
   const char *name = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(frame, "tool"));
   char *arguments_json = cJSON_PrintUnformatted(cJSON_GetObjectItemCaseSensitive(frame, "args"));
   /* Keep this unit fast and focused on RPC framing. Full dispatch-path coverage lives in
    * unit-test-agent, which links the real agent_tools_dispatch implementation. */
   g_dispatch_calls++;
   if (g_dispatch_blocked)
   {
      atomic_store(&dispatch_entered, 1);
      while (!atomic_load(&dispatch_released))
         usleep(1000);
   }
   *is_error = g_dispatch_error;
   if (g_dispatch_result)
   {
      free(arguments_json);
      return safe_strdup(g_dispatch_result);
   }
   if (g_dispatch_error)
   {
      free(arguments_json);
      return safe_strdup(
          "{\"status\":\"refused\",\"reason\":\"fixture\",\"dispatch_allowed\":false}");
   }
   char buf[2048];
   snprintf(buf, sizeof(buf), "{\"tool\":\"%s\",\"args\":%s}", name ? name : "",
            arguments_json ? arguments_json : "{}");
   free(arguments_json);
   return safe_strdup(buf);
}

/* Framing-only fixture: permission semantics use the real tools owner in
 * test_agent_mcp_permissions.c, including the joined RPC worker. */
int agent_tools_scope_capture_owned(agent_tools_scope_snapshot_t *out)
{
   memset(out, 0, sizeof *out);
   return g_scope_capture_refused ? -1 : 0;
}
void agent_tools_scope_restore(const agent_tools_scope_snapshot_t *scope)
{
   (void)scope;
}
void agent_tools_scope_destroy_owned(agent_tools_scope_snapshot_t *scope)
{
   memset(scope, 0, sizeof *scope);
}

/* No authenticated host task in this framing-only fixture. Real context
 * propagation is exercised through unit-test-agent's actual dispatcher. */
int server_session_capture_retain_current(const char *session, const char *actor,
                                          server_session_capture_t **out)
{
   (void)session;
   (void)actor;
   *out = NULL;
   return 0;
}
server_session_capture_t *server_session_capture_bind(server_session_capture_t *capture)
{
   assert(!capture);
   return NULL;
}
void server_session_capture_release(server_session_capture_t *capture)
{
   assert(!capture);
}
void request_context_snapshot(request_context_snapshot_t *out)
{
   memset(out, 0, sizeof *out);
}
void request_context_restore(const request_context_snapshot_t *scope)
{
   (void)scope;
}
void request_context_snapshot_clear(request_context_snapshot_t *scope)
{
   memset(scope, 0, sizeof *scope);
}
void request_context_clear(void)
{
}
int agent_source_authority_tls_capture_checked(agent_source_authority_snapshot_t *out)
{
   memset(out, 0, sizeof *out);
   return g_source_capture_refused ? -1 : 0;
}
void agent_source_authority_tls_restore(agent_source_authority_snapshot_t *scope)
{
   scope->paths = NULL;
}
void agent_source_authority_snapshot_destroy(agent_source_authority_snapshot_t *scope)
{
   free(scope->paths);
   memset(scope, 0, sizeof *scope);
}
const workspace_provider_t *workspace_provider_active(void)
{
   return NULL;
}
void workspace_provider_set_active(const workspace_provider_t *provider)
{
   (void)provider;
}
void run_cmd_set_cwd(const char *cwd)
{
   (void)cwd;
}
void agent_set_request_session(const char *sid)
{
   (void)sid;
}
const char *agent_get_request_vault_principal(void)
{
   return NULL;
}
void agent_set_request_vault_principal(const char *principal)
{
   (void)principal;
}
atomic_int *agent_get_request_cancel(void)
{
   return request_cancel;
}
void agent_set_request_cancel(atomic_int *flag)
{
   request_cancel = flag;
}
int agent_request_cancelled(void)
{
   if (request_cancel && atomic_load(request_cancel))
      return 1;
   for (const agent_request_cancel_scope_t *scope = request_cancel_scope; scope;
        scope = scope->parent)
      if (scope->flag && atomic_load(scope->flag))
         return 1;
   return 0;
}
const agent_request_cancel_scope_t *agent_request_cancel_scope_current(void)
{
   return request_cancel_scope;
}
const agent_request_cancel_scope_t *
agent_request_cancel_scope_bind(const agent_request_cancel_scope_t *scope)
{
   const agent_request_cancel_scope_t *previous = request_cancel_scope;
   request_cancel_scope = scope;
   return previous;
}
int fixture_job_get(int id, db1_agent_job_t *out)
{
   (void)id;
   (void)out;
   abort();
}
void fixture_job_free(db1_agent_job_t *out)
{
   (void)out;
}
void fixture_job_heartbeat_ext(int id, const char *tool, int count)
{
   (void)id;
   (void)tool;
   (void)count;
   abort();
}

/* Session-file persistence is outside this framing-only fixture. */
int platform_getppid(void)
{
   return 0;
}
int platform_mkdir_p(const char *path, int mode)
{
   (void)path;
   (void)mode;
   return -1;
}

const char *agent_tools_parent_write_guard_root(void)
{
   return NULL;
}

const char *run_cmd_get_cwd(void)
{
   return NULL;
}

char *safe_strdup(const char *s)
{
   char *out = strdup(s ? s : "");
   assert(out != NULL);
   return out;
}

static cJSON *parse_json_or_die(const char *text)
{
   cJSON *json = cJSON_Parse(text);
   assert(json != NULL);
   return json;
}

static void test_script_env_scrub(void)
{
   assert(setenv("AIMEE_RPC_TOKEN", "parent-cap", 1) == 0);
   assert(setenv("AIMEE_HOME", "/tmp/parent-aimee-home", 1) == 0);
   assert(setenv("AIMEE_SESSION_ID", "script-session", 1) == 0);
   assert(setenv("OPENAI_API_KEY", "parent-secret", 1) == 0);
   char *parent_result = tool_execute_script(
       "python",
       "import os; print(os.getenv('AIMEE_RPC_TOKEN')); print(os.getenv('AIMEE_HOME')); "
       "print(os.getenv('AIMEE_SESSION_ID'))",
       5, NULL, NULL);
   assert(parent_result != NULL);
   cJSON *parent_json = parse_json_or_die(parent_result);
   assert(cJSON_GetObjectItem(parent_json, "exit_code")->valueint == 0);
   const char *parent_stdout = cJSON_GetObjectItem(parent_json, "stdout")->valuestring;
   assert(strstr(parent_stdout, "parent-cap") == NULL);
   assert(strstr(parent_stdout, "\nNone\nscript-session\n") != NULL);
   cJSON_Delete(parent_json);
   free(parent_result);

   char *result = tool_execute_script(
       "python",
       "import os; print(os.getenv('AIMEE_RPC_SOCKET')); print(os.getenv('AIMEE_RPC_TOKEN')); "
       "print(os.getenv('OPENAI_API_KEY')); "
       "print('OPENAI_API_KEY' in os.environ); print('CUSTOM_TOKEN' in os.environ)",
       5, NULL,
       "{\"AIMEE_RPC_SOCKET\":\"/tmp/aimee.sock\",\"AIMEE_RPC_TOKEN\":\"cap\","
       "\"OPENAI_API_KEY\":\"x\",\"CUSTOM_TOKEN\":\"y\"}");
   assert(result != NULL);
   cJSON *json = parse_json_or_die(result);
   assert(cJSON_GetObjectItem(json, "exit_code")->valueint == 0);
   const char *stdout_text = cJSON_GetObjectItem(json, "stdout")->valuestring;
   assert(strstr(stdout_text, "/tmp/aimee.sock") == NULL);
   assert(strstr(stdout_text, "\ncap\n") == NULL);
   assert(strstr(stdout_text, "\nNone\nFalse\nFalse") != NULL);
   assert(cJSON_IsFalse(cJSON_GetObjectItem(json, "stdout_truncated")));
   cJSON_Delete(json);
   free(result);
   unsetenv("AIMEE_RPC_TOKEN");
   unsetenv("AIMEE_HOME");
   unsetenv("AIMEE_SESSION_ID");
   unsetenv("OPENAI_API_KEY");
}

static void test_python_rpc_allowed_tool(void)
{
   g_dispatch_calls = 0;
   char *result = tool_execute_script(
       "python",
       "import json, aimee_tools\n"
       "r = aimee_tools.call('read_file', {'path': '/tmp/example.txt', 'limit': 3})\n"
       "print(json.dumps(r, sort_keys=True))\n",
       5, NULL, NULL);
   assert(result != NULL);
   cJSON *json = parse_json_or_die(result);
   assert(cJSON_GetObjectItem(json, "exit_code")->valueint == 0);
   assert(cJSON_GetObjectItem(json, "script_tool_calls")->valueint == 1);
   assert(g_dispatch_calls == 1);
   const char *stdout_text = cJSON_GetObjectItem(json, "stdout")->valuestring;
   assert(strstr(stdout_text, "\"ok\": true") != NULL);
   assert(strstr(stdout_text, "\"tool\": \"read_file\"") != NULL);
   cJSON_Delete(json);
   free(result);
}

static void test_python_rpc_structured_refusal(void)
{
   g_dispatch_calls = 0;
   g_dispatch_error = 1;
   char *result =
       tool_execute_script("python",
                           "import aimee_tools\n"
                           "r = aimee_tools.call('read_file', {'path': '/tmp/example.txt'})\n"
                           "assert r['ok'] is False\n"
                           "assert r['result']['status'] == 'refused'\n"
                           "assert 'fixture' in r['error']\n"
                           "print('refusal retained')\n",
                           5, NULL, NULL);
   g_dispatch_error = 0;
   assert(result);
   cJSON *json = parse_json_or_die(result);
   assert(cJSON_GetObjectItem(json, "exit_code")->valueint == 0 && g_dispatch_calls == 1);
   assert(strstr(cJSON_GetObjectItem(json, "stdout")->valuestring, "refusal retained"));
   cJSON_Delete(json);
   free(result);
}

static void test_bash_rpc_shim(void)
{
   g_dispatch_calls = 0;
   char *result = tool_execute_script(
       "bash", "aimee tool read_file '{\"path\":\"/tmp/bash.txt\"}'\n", 5, NULL, NULL);
   assert(result != NULL);
   cJSON *json = parse_json_or_die(result);
   assert(cJSON_GetObjectItem(json, "exit_code")->valueint == 0);
   assert(cJSON_GetObjectItem(json, "script_tool_calls")->valueint == 1);
   assert(g_dispatch_calls == 1);
   const char *stdout_text = cJSON_GetObjectItem(json, "stdout")->valuestring;
   assert(strstr(stdout_text, "\"ok\":true") != NULL);
   cJSON_Delete(json);
   free(result);
}

static void test_rpc_policy_denial(void)
{
   g_dispatch_calls = 0;
   char *result = tool_execute_script(
       "python",
       "import json, aimee_tools\n"
       "print(json.dumps(aimee_tools.call('write_file', {'path':'x','content':'y'}), "
       "sort_keys=True))\n",
       5, NULL, NULL);
   assert(result != NULL);
   cJSON *json = parse_json_or_die(result);
   assert(cJSON_GetObjectItem(json, "exit_code")->valueint == 0);
   assert(cJSON_GetObjectItem(json, "script_tool_calls")->valueint == 0);
   assert(cJSON_GetObjectItem(json, "script_policy_denials")->valueint == 1);
   assert(g_dispatch_calls == 0);
   const char *stdout_text = cJSON_GetObjectItem(json, "stdout")->valuestring;
   assert(strstr(stdout_text, "\"code\": \"policy_denied\"") != NULL);
   cJSON_Delete(json);
   free(result);
}

static void test_rpc_descriptor_is_the_only_capability(void)
{
   g_dispatch_calls = 0;
   char *result = tool_execute_script(
       "python",
       "import json, os, struct\n"
       "assert os.environ.get('AIMEE_RPC_TOKEN') is None\n"
       "assert os.environ.get('AIMEE_RPC_SOCKET') is None\n"
       "frame = json.dumps({'id': 1, 'tool': 'read_file', 'args': {'path':'x'}, "
       "'hmac': 'obsolete-and-ignored'}, separators=(',', ':')).encode()\n"
       "fd = os.dup(int(os.environ['AIMEE_RPC_FD']))\n"
       "os.write(fd, struct.pack('!I', len(frame)) + frame)\n"
       "hdr = os.read(fd, 4)\n"
       "size = struct.unpack('!I', hdr)[0]\n"
       "print(os.read(fd, size).decode())\n"
       "os.close(fd)\n",
       5, NULL, NULL);
   assert(result != NULL);
   cJSON *json = parse_json_or_die(result);
   assert(cJSON_GetObjectItem(json, "exit_code")->valueint == 0);
   assert(cJSON_GetObjectItem(json, "script_tool_calls")->valueint == 1);
   assert(cJSON_GetObjectItem(json, "script_hmac_failures")->valueint == 0);
   assert(g_dispatch_calls == 1);
   const char *stdout_text = cJSON_GetObjectItem(json, "stdout")->valuestring;
   assert(strstr(stdout_text, "\"ok\":true") != NULL);
   cJSON_Delete(json);
   free(result);
}

static void test_nine_tool_fan_in(void)
{
   g_dispatch_calls = 0;
   const char *body = "import aimee_tools\n"
                      "for i in range(9):\n"
                      "    r = aimee_tools.call('read_file', {'path': str(i)})\n"
                      "    assert r['ok']\n"
                      "print('done')\n";
   char *result = tool_execute_script("python", body, 5, NULL, NULL);
   assert(result != NULL);
   cJSON *json = parse_json_or_die(result);
   assert(cJSON_GetObjectItem(json, "exit_code")->valueint == 0);
   assert(cJSON_GetObjectItem(json, "script_tool_calls")->valueint == 9);
   assert(g_dispatch_calls == 9);
   assert(strstr(cJSON_GetObjectItem(json, "stdout")->valuestring, "done") != NULL);
   cJSON_Delete(json);
   free(result);
}

static void test_script_timeout(void)
{
   char *result = tool_execute_script("bash", "sleep 2", 1, NULL, NULL);
   assert(result != NULL);
   cJSON *json = parse_json_or_die(result);
   assert(cJSON_GetObjectItem(json, "exit_code")->valueint == -1);
   assert(cJSON_IsTrue(cJSON_GetObjectItem(json, "timed_out")));
   cJSON_Delete(json);
   free(result);
}

static void *cancel_ready_script(void *context)
{
   const char *ready = context;
   for (int tick = 0; tick < 2000; tick++)
   {
      if (access(ready, F_OK) == 0)
         return (void *)1;
      usleep(1000);
   }
   return NULL;
}

typedef struct
{
   const char *ready;
   atomic_int *flag;
} script_canceller_t;

static void *cancel_after_ready(void *context)
{
   script_canceller_t *cancel = context;
   void *ready = cancel_ready_script((void *)cancel->ready);
   atomic_store(cancel->flag, 1);
   return ready;
}

static void test_running_script_cancellation(void)
{
   char directory[MAX_PATH_LEN];
   const char *temporary = getenv("TMPDIR");
   snprintf(directory, sizeof directory, "%s/script-cancel-XXXXXX",
            temporary && temporary[0] ? temporary : "/tmp");
   char *made = mkdtemp(directory);
   assert(made);
   char ready[MAX_PATH_LEN], body[MAX_PATH_LEN * 2];
   snprintf(ready, sizeof ready, "%s/ready", directory);
   snprintf(body, sizeof body,
            "import signal,time\n"
            "signal.signal(signal.SIGTERM, signal.SIG_IGN)\n"
            "open('%s', 'w').close()\n"
            "time.sleep(2)\n",
            ready);
   atomic_int cancel = 0;
   request_cancel = &cancel;
   script_canceller_t context = {.ready = ready, .flag = &cancel};
   pthread_t thread;
   int created = pthread_create(&thread, NULL, cancel_after_ready, &context);
   assert(!created);
   char *result = tool_execute_script("python", body, 5, NULL, NULL);
   void *saw_ready = NULL;
   pthread_join(thread, &saw_ready);
   request_cancel = NULL;
   assert(saw_ready);
   cJSON *json = parse_json_or_die(result);
   assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(json, "cancelled")));
   assert(!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(json, "timed_out")));
   assert(cJSON_GetObjectItemCaseSensitive(json, "exit_code")->valueint == -1);
   assert(cJSON_GetObjectItemCaseSensitive(json, "duration_ms")->valuedouble < 1500);
   cJSON_Delete(json);
   free(result);
   unlink(ready);
   rmdir(directory);
}

static void test_closed_output_keeps_timeout(void)
{
   char *result = tool_execute_script(
       "python", "import os,time; os.close(1); os.close(2); time.sleep(2)", 1, NULL, NULL);
   cJSON *json = parse_json_or_die(result);
   assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(json, "timed_out")));
   assert(cJSON_GetObjectItemCaseSensitive(json, "exit_code")->valueint == -1);
   assert(cJSON_GetObjectItemCaseSensitive(json, "duration_ms")->valuedouble < 1800);
   cJSON_Delete(json);
   free(result);
}

static void test_rpc_result_bounds_and_fidelity(void)
{
   char *large = malloc(100 * 1024 + 1);
   assert(large);
   for (int error = 0; error < 2; error++)
      for (int escaped = 0; escaped < 2; escaped++)
      {
         size_t length = escaped ? 25000 : 100 * 1024;
         memset(large, escaped ? 1 : 'x', length);
         large[length] = 0;
         g_dispatch_result = large;
         g_dispatch_error = error;
         char body[1024];
         snprintf(body, sizeof body,
                  "import aimee_tools, json\n"
                  "r = aimee_tools.call('read_file', {'path': 'fixture'})\n"
                  "assert r['ok'] is %s\n"
                  "assert r['result_truncated'] is True\n"
                  "assert r['result_bytes'] == %zu\n"
                  "assert isinstance(r['result'], str) and len(r['result']) > 0\n"
                  "assert r['result'] == chr(%d) * len(r['result'])\n"
                  "assert len(json.dumps(r).encode()) <= 65536\n"
                  "assert aimee_tools.call('read_file', {'path': 'again'})['result_truncated']\n"
                  "print('bounded result received')\n",
                  error ? "False" : "True", length, escaped ? 1 : 'x');
         char *result = tool_execute_script("python", body, 5, NULL, NULL);
         cJSON *reply = parse_json_or_die(result);
         assert(cJSON_GetObjectItemCaseSensitive(reply, "exit_code")->valueint == 0);
         assert(strstr(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(reply, "stdout")),
                       "bounded result received"));
         cJSON_Delete(reply);
         free(result);
      }
   g_dispatch_error = 0;
   for (int unicode = 0; unicode < 2; unicode++)
   {
      if (unicode)
      {
         large[0] = 'x';
         for (size_t i = 1; i < 100000; i += 3)
            memcpy(large + i, "\xe2\x82\xac", 3);
         large[100000] = 0;
      }
      else
      {
         memset(large, 'x', 64000);
         large[64000] = 0;
      }
      g_dispatch_result = large;
      char *result =
          tool_execute_script("python",
                              "import aimee_tools\n"
                              "r = aimee_tools.call('read_file', {'path': 'fixture'})\n"
                              "assert r['ok']\n"
                              "if r.get('result_truncated'):\n"
                              "    assert r['result_bytes'] == 100000\n"
                              "    assert r['result'] == 'x' + chr(8364) * (len(r['result']) - 1)\n"
                              "else:\n"
                              "    assert r['result'] == 'x' * 64000\n",
                              5, NULL, NULL);
      cJSON *reply = parse_json_or_die(result);
      assert(cJSON_GetObjectItemCaseSensitive(reply, "exit_code")->valueint == 0);
      cJSON_Delete(reply);
      free(result);
   }
   free(large);
   const char *ambiguous[] = {"123 trailing", "{\"same\":1,\"same\":2}"};
   for (size_t i = 0; i < sizeof ambiguous / sizeof ambiguous[0]; i++)
   {
      g_dispatch_result = ambiguous[i];
      char *result =
          tool_execute_script("python",
                              "import aimee_tools\n"
                              "r = aimee_tools.call('read_file', {'path': 'fixture'})\n"
                              "assert r['ok'] and isinstance(r['result'], str)\n"
                              "assert r['result'] in ['123 trailing', '{\"same\":1,\"same\":2}']\n",
                              5, NULL, NULL);
      cJSON *reply = parse_json_or_die(result);
      assert(cJSON_GetObjectItemCaseSensitive(reply, "exit_code")->valueint == 0);
      cJSON_Delete(reply);
      free(result);
   }
   g_dispatch_result = NULL;
}

static void test_script_stdout_cap(void)
{
   char *result = tool_execute_script("python", "print('x' * 60000)", 5, NULL, NULL);
   assert(result != NULL);
   cJSON *json = parse_json_or_die(result);
   int exit_code = cJSON_GetObjectItem(json, "exit_code")->valueint;
   assert(exit_code == 0);
   assert(cJSON_IsTrue(cJSON_GetObjectItem(json, "stdout_truncated")));
   assert(strlen(cJSON_GetObjectItem(json, "stdout")->valuestring) == 50 * 1024);
   cJSON_Delete(json);
   free(result);
}

static void test_script_stderr_cap(void)
{
   char *result =
       tool_execute_script("python", "import sys; sys.stderr.write('e' * 20000)", 5, NULL, NULL);
   assert(result != NULL);
   cJSON *json = parse_json_or_die(result);
   int exit_code = cJSON_GetObjectItem(json, "exit_code")->valueint;
   assert(exit_code == 0);
   assert(cJSON_IsTrue(cJSON_GetObjectItem(json, "stderr_truncated")));
   assert(strlen(cJSON_GetObjectItem(json, "stderr")->valuestring) == 10 * 1024);
   cJSON_Delete(json);
   free(result);
}

static void test_permission_capture_failure_refuses_script(void)
{
   for (int source = 0; source < 2; source++)
   {
      g_scope_capture_refused = !source;
      g_source_capture_refused = source;
      g_dispatch_calls = 0;
      char *result = tool_execute_script("python", "print('must-not-start')", 5, NULL, NULL);
      g_scope_capture_refused = g_source_capture_refused = 0;
      cJSON *reply = parse_json_or_die(result);
      assert(cJSON_GetObjectItemCaseSensitive(reply, "exit_code")->valueint == -1);
      const char *error = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(reply, "stderr"));
      const char *output = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(reply, "stdout"));
      assert(error && strstr(error, "RPC setup failed") && output && !output[0] &&
             !g_dispatch_calls);
      cJSON_Delete(reply);
      free(result);
   }
}

static void test_rpc_strict_json_frames(void)
{
   g_dispatch_calls = 0;
   char *result = tool_execute_script(
       "python",
       "import os, struct, json\n"
       "fd = int(os.environ['AIMEE_RPC_FD'])\n"
       "frames = [b'{\"id\":1,\"tool\":\"read_file\",\"args\":{}} trailing',\n"
       "          b'{\"id\":1,\"tool\":\"read_file\",\"args\":{\"path\":\"a\",\"path\":\"b\"}}',\n"
       "          b'{\"id\":1,\"tool\":\"read_file\",\"args\":{}}' + bytes([0]) + b'trailing']\n"
       "for frame in frames:\n"
       "    os.write(fd, struct.pack('!I', len(frame)) + frame)\n"
       "    size = struct.unpack('!I', os.read(fd, 4))[0]\n"
       "    reply = json.loads(os.read(fd, size))\n"
       "    assert reply['ok'] is False and reply['code'] == 'bad_json'\n"
       "frame = b'{\"id\":1,\"tool\":\"read_file\",\"args\":{},\"args_json\":\"{}\"}'\n"
       "os.write(fd, struct.pack('!I', len(frame)) + frame)\n"
       "size = struct.unpack('!I', os.read(fd, 4))[0]\n"
       "reply = json.loads(os.read(fd, size))\n"
       "assert reply['ok'] is False and reply['code'] == 'bad_frame'\n"
       "print('strict frames refused')\n",
       5, NULL, NULL);
   cJSON *reply = parse_json_or_die(result);
   assert(cJSON_GetObjectItemCaseSensitive(reply, "exit_code")->valueint == 0 && !g_dispatch_calls);
   const char *output = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(reply, "stdout"));
   assert(output && strstr(output, "strict frames refused"));
   cJSON_Delete(reply);
   free(result);
}

static void test_bash_shim_preserves_raw_arguments(void)
{
   g_dispatch_calls = 0;
   char *result =
       tool_execute_script("bash",
                           "aimee tool read_file '{\"path\":\"first\"} trailing'\n"
                           "aimee tool read_file '{\"path\":\"first\",\"path\":\"second\"}'\n"
                           "aimee tool read_file '{\"path\":\"first\"},\"hmac\":\"ignored\"'\n",
                           5, NULL, NULL);
   cJSON *reply = parse_json_or_die(result);
   assert(cJSON_GetObjectItemCaseSensitive(reply, "exit_code")->valueint == 0);
   assert(g_dispatch_calls == 0);
   const char *output = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(reply, "stdout"));
   assert(output && strstr(output, "bad_json"));
   cJSON_Delete(reply);
   free(result);
   result =
       tool_execute_script("bash",
                           "aimee tool read_file "
                           "'{\"path\":\"fixture\",\"weight\":123.123456789012345678901234567890}'",
                           5, NULL, NULL);
   reply = parse_json_or_die(result);
   output = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(reply, "stdout"));
   assert(cJSON_GetObjectItemCaseSensitive(reply, "exit_code")->valueint == 0);
   assert(g_dispatch_calls == 1 && output && strstr(output, "123.123456789012345678901234567890"));
   cJSON_Delete(reply);
   free(result);
   g_dispatch_calls = 0;
   result = tool_execute_script("python",
                                "import aimee_tools\n"
                                "r = aimee_tools.call('read_file', [])\n"
                                "assert r['ok'] is False and r['code'] == 'bad_frame'\n",
                                5, NULL, NULL);
   reply = parse_json_or_die(result);
   assert(cJSON_GetObjectItemCaseSensitive(reply, "exit_code")->valueint == 0 && !g_dispatch_calls);
   cJSON_Delete(reply);
   free(result);
}

static atomic_int rpc_stopped;
static void *stop_partial_rpc(void *context)
{
   script_rpc_stop(context);
   atomic_store(&rpc_stopped, 1);
   return NULL;
}

static void test_rpc_stop_interrupts_partial_frame(void)
{
   script_rpc_server_t server;
   int prepared = script_rpc_prepare(&server, NULL);
   assert(!prepared);
   int started = script_rpc_start(&server);
   assert(!started);
   uint32_t length = htonl(100);
   ssize_t header = write(server.client_fd, &length, sizeof length);
   ssize_t body = write(server.client_fd, "{", 1);
   assert(header == sizeof length && body == 1);
   int pending = 1;
   for (int tick = 0; tick < 100 && pending; tick++)
   {
      int queried = ioctl(server.listen_fd, FIONREAD, &pending);
      assert(!queried);
      if (pending)
         usleep(10000);
   }
   assert(!pending); /* The worker consumed a header and now waits for the absent body. */
   atomic_store(&rpc_stopped, 0);
   pthread_t stopper;
   int created = pthread_create(&stopper, NULL, stop_partial_rpc, &server);
   assert(!created);
   for (int tick = 0; tick < 100 && !atomic_load(&rpc_stopped); tick++)
      usleep(10000);
   assert(atomic_load(&rpc_stopped));
   pthread_join(stopper, NULL);
   script_rpc_cleanup(&server);
}

static void test_rpc_closed_client_cannot_signal_host(void)
{
   struct sigaction original, action = {.sa_handler = SIG_DFL};
   sigemptyset(&action.sa_mask);
   int configured = sigaction(SIGPIPE, &action, &original);
   assert(!configured);
   script_rpc_server_t server;
   int prepared = script_rpc_prepare(&server, NULL);
   assert(!prepared);
   g_dispatch_blocked = 1;
   atomic_store(&dispatch_entered, 0);
   atomic_store(&dispatch_released, 0);
   int started = script_rpc_start(&server);
   assert(!started);
   const char frame[] = "{\"id\":1,\"tool\":\"read_file\",\"args\":{\"path\":\"fixture\"}}";
   uint32_t length = htonl(sizeof frame - 1);
   ssize_t header = write(server.client_fd, &length, sizeof length);
   ssize_t body = write(server.client_fd, frame, sizeof frame - 1);
   assert(header == sizeof length && body == sizeof frame - 1);
   for (int tick = 0; tick < 100 && !atomic_load(&dispatch_entered); tick++)
      usleep(10000);
   assert(atomic_load(&dispatch_entered));
   close(server.client_fd);
   server.client_fd = -1;
   atomic_store(&dispatch_released, 1);
   script_rpc_stop(&server);
   script_rpc_cleanup(&server);
   g_dispatch_blocked = 0;
   int restored = sigaction(SIGPIPE, &original, NULL);
   assert(!restored);
}

int main(void)
{
   test_bash_shim_preserves_raw_arguments();
   test_rpc_result_bounds_and_fidelity();
   test_running_script_cancellation();
   test_closed_output_keeps_timeout();
   test_rpc_closed_client_cannot_signal_host();
   test_rpc_stop_interrupts_partial_frame();
   test_rpc_strict_json_frames();
   test_permission_capture_failure_refuses_script();
   test_script_env_scrub();
   test_python_rpc_allowed_tool();
   test_python_rpc_structured_refusal();
   test_bash_rpc_shim();
   test_rpc_policy_denial();
   test_rpc_descriptor_is_the_only_capability();
   test_nine_tool_fan_in();
   test_script_timeout();
   test_script_stdout_cap();
   test_script_stderr_cap();
   printf("script_runner: all tests passed\n");
   return 0;
}
