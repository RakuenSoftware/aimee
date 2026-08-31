#include "aimee.h"
#include <aimee/tools/agent_tools.h>
#include "cJSON.h"
#include "log.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_dispatch_calls = 0;

char *dispatch_tool_call_ctx(const char *name, const char *arguments_json, int timeout_ms)
{
   /* Keep this unit fast and focused on RPC framing. Full dispatch-path coverage lives in
    * unit-test-agent, which links the real agent_tools_dispatch implementation. */
   g_dispatch_calls++;
   char buf[2048];
   snprintf(buf, sizeof(buf), "{\"tool\":\"%s\",\"args\":%s}", name ? name : "",
            arguments_json ? arguments_json : "{}");
   return safe_strdup(buf);
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

int main(void)
{
   test_script_env_scrub();
   test_python_rpc_allowed_tool();
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
