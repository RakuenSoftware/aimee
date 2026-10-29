/* Isolated real tools/policy owners for the joined script execution fixtures. */
#include "aimee.h"
#include "../modules/tools/agent_tools_internal.h"
#include "agent_config.h"
#include "config_client.h"
#include "sandbox.h"
#include <aimee/tools/agent_tools.h>
#include "request_context.h"
#include "module_stage_adapters.h"
#include "platform_test_util.h"
#include <aimee/audit/obs_bus.h>
#include <aimee/tools/module_api.h>
#include <aimee/execution-policy/module_api.h>
#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif
#include <unistd.h>

void test_script_rpc_keeps_permission_scope(void);
void test_script_rpc_keeps_host_context(void);
static pid_t modules[2] = {-1, -1};

static void stop_modules(void)
{
   for (int i = 0; i < 2; i++)
      if (modules[i] > 0)
      {
         kill(modules[i], SIGTERM);
         while (waitpid(modules[i], NULL, 0) < 0 && errno == EINTR)
         {
         }
         modules[i] = -1;
      }
   obs_bus_stop();
}

static void copy_module(const char *source, const char *destination)
{
   FILE *in = fopen(source, "rb"), *out = fopen(destination, "wb");
   assert(in && out);
   unsigned char bytes[65536];
   size_t count;
   while ((count = fread(bytes, 1, sizeof bytes, in)) != 0)
   {
      size_t copied = fwrite(bytes, 1, count, out);
      assert(copied == count);
   }
   int failed = ferror(in), closed_in = fclose(in), closed_out = fclose(out);
   assert(!failed && !closed_in && !closed_out);
   int mode = chmod(destination, 0700);
   assert(!mode);
}

static void probe_read(const char *root, int permitted, const char *reason)
{
   char body[MAX_PATH_LEN * 2];
   snprintf(body, sizeof body,
            "import json, aimee_tools\n"
            "print(json.dumps(aimee_tools.call('read_file', {'filepath': '%s/evidence.txt'})))\n",
            root);
   char *result = tool_execute_script("python", body, 30, root, NULL);
   cJSON *reply = result ? cJSON_Parse(result) : NULL;
   assert(reply && cJSON_GetObjectItemCaseSensitive(reply, "exit_code")->valueint == 0);
   const char *output = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(reply, "stdout"));
   assert(output && strstr(output, permitted ? "\"ok\": true" : "\"ok\": false"));
   assert((strstr(output, "SCRIPT_POLICY_PRIVATE_EVIDENCE") != NULL) == permitted);
   if (reason)
      assert(strstr(output, reason));
   cJSON_Delete(reply);
   free(result);
}

static void policy_and_absence(const char *root)
{
   char path[MAX_PATH_LEN];
   snprintf(path, sizeof path, "%s/evidence.txt", root);
   FILE *file = fopen(path, "w");
   assert(file);
   fputs("SCRIPT_POLICY_PRIVATE_EVIDENCE", file);
   int closed = fclose(file);
   assert(!closed);
   probe_read(root, 1, NULL);
   cJSON *policy = cJSON_CreateObject(), *rules = cJSON_AddArrayToObject(policy, "tool_rules");
   cJSON *rule = cJSON_CreateObject();
   cJSON_AddItemToArray(rules, rule);
   cJSON_AddStringToObject(rule, "path_prefix", root);
   cJSON *allowed = cJSON_AddArrayToObject(rule, "allowed_tools");
   cJSON_AddItemToArray(allowed, cJSON_CreateString("list_files"));
   char *text = cJSON_PrintUnformatted(policy);
   cJSON_Delete(policy);
   assert(text);
   snprintf(path, sizeof path, "%s/policy.json", root);
   file = fopen(path, "w");
   assert(file);
   fputs(text, file);
   closed = fclose(file);
   free(text);
   assert(!closed);
   probe_read(root, 0, "not allowed for path");
   unlink(path);
   probe_read(root, 1, NULL);
   kill(modules[1], SIGTERM);
   pid_t stopped = waitpid(modules[1], NULL, 0);
   assert(stopped == modules[1]);
   modules[1] = -1;
   probe_read(root, 0, "execution-policy");
   kill(modules[0], SIGTERM);
   stopped = waitpid(modules[0], NULL, 0);
   assert(stopped == modules[0]);
   modules[0] = -1;
   probe_read(root, 0, "metadata");
}

static void nested_tool_stop(const char *root)
{
   cJSON *previous = config_client_snapshot_copy();
   assert(previous);
   cJSON *sets = cJSON_Parse("{\"timeout_script\":{\"tools\":[\"bash\"]}}");
   cJSON *script = cJSON_Parse("{\"allowed_tools\":\"timeout_script\"}");
   int configured = config_client_set_value("toolsets", sets);
   int selected = config_client_set_value("script", script);
   assert(!configured && !selected);
   sandbox_set_mode_override_for_test(SANDBOX_MODE_OFF);
   atomic_int parent_cancel = 0;
   atomic_int *previous_cancel = agent_get_request_cancel();
   agent_set_request_cancel(&parent_cancel);
   char body[MAX_PATH_LEN * 2], ready[MAX_PATH_LEN];
   snprintf(ready, sizeof ready, "%s/nested-ready", root);
   snprintf(body, sizeof body,
            "import aimee_tools\n"
            "print(aimee_tools.call('bash', {'command':\"exec 1>&- 2>&-; "
            "trap '' TERM; : > '%s'; sleep 2\"}))\n",
            ready);
   char *result = tool_execute_script("python", body, 1, root, NULL);
   cJSON *reply = result ? cJSON_Parse(result) : NULL;
   assert(reply && access(ready, F_OK) == 0);
   assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(reply, "timed_out")));
   assert(cJSON_GetObjectItemCaseSensitive(reply, "duration_ms")->valuedouble < 1800);
   assert(!atomic_load(&parent_cancel) && agent_get_request_cancel() == &parent_cancel);
   cJSON_Delete(reply);
   free(result);
   unlink(ready);
   agent_set_request_cancel(previous_cancel);
   sandbox_set_mode_override_for_test(-1);
   const cJSON *old_sets = cJSON_GetObjectItemCaseSensitive(previous, "toolsets");
   const cJSON *old_script = cJSON_GetObjectItemCaseSensitive(previous, "script");
   configured = config_client_set_value("toolsets", old_sets ? cJSON_Duplicate(old_sets, 1)
                                                             : cJSON_CreateObject());
   selected = config_client_set_value("script", old_script ? cJSON_Duplicate(old_script, 1)
                                                           : cJSON_CreateObject());
   assert(!configured && !selected);
   cJSON_Delete(previous);
   puts("script RPC: local timeout cancels an executing tool without cancelling its parent");
}

int script_rpc_admission_fixture_main(void)
{
   char root[MAX_PATH_LEN], policy[MAX_PATH_LEN], socket_path[MAX_PATH_LEN];
   snprintf(root, sizeof root, "%s/sra.XXXXXX", platform_tmpdir());
   char *created = platform_mkdtemp(root);
   assert(created);
   const char *binary = getenv("AIMEE_TEST_MODULE_BIN");
   assert(binary && binary[0]);
   int home = setenv("HOME", root, 1), aimee_home = setenv("AIMEE_HOME", root, 1);
   int config_home = setenv("AIMEE_CONFIG_TEST_HOST_HOME", root, 1);
   assert(!home && !aimee_home && !config_home);
   snprintf(policy, sizeof policy, "%s/grants", root);
   snprintf(socket_path, sizeof socket_path, "%s/b.sock", root);
   int made = mkdir(policy, 0700);
   assert(!made);
   const char *names[] = {"tools", "execution-policy"};
   uint32_t kinds[] = {AIMEE_TOOLS_EVENT_DISPATCH, AIMEE_EXECUTION_POLICY_EVENT_TOOL};
   char executable[2][MAX_PATH_LEN];
   for (int i = 0; i < 2; i++)
   {
      snprintf(executable[i], sizeof executable[i], "%s/aimee-module-%s", root, names[i]);
      copy_module(binary, executable[i]);
      char path[MAX_PATH_LEN];
      snprintf(path, sizeof path, "%s/%s.grant", policy, names[i]);
      FILE *grant = fopen(path, "w");
      assert(grant);
      fprintf(grant,
              "version=1\nprincipal_class=1\nprincipal_ref=%u\nuid=self\n"
              "executable=%s\nserve=%u",
              (kinds[i] - 4096u) / 256u, executable[i], kinds[i]);
      if (!i)
         fprintf(grant, ",%u", AIMEE_TOOLS_EVENT_ACTION_RESOURCE);
      fputc('\n', grant);
      int closed = fclose(grant);
      assert(!closed);
   }
   int configured = obs_bus_configure_module_runtime(socket_path, policy);
   int started = configured ? -1 : obs_bus_start();
   assert(!configured && !started);
   atexit(stop_modules);
#ifdef __linux__
   pid_t parent = getpid();
#endif
   for (int i = 0; i < 2; i++)
   {
      modules[i] = fork();
      assert(modules[i] >= 0);
      if (!modules[i])
      {
#ifdef __linux__
         prctl(PR_SET_PDEATHSIG, SIGKILL);
         if (getppid() != parent)
            _exit(126);
#endif
         execl(executable[i], executable[i], socket_path, (char *)NULL);
         _exit(127);
      }
   }
   for (int tick = 0;
        tick < 200 && (!obs_bus_module_available(kinds[0]) || !obs_bus_module_available(kinds[1]));
        tick++)
   {
      for (int i = 0; i < 2; i++)
      {
         int status;
         pid_t exited = waitpid(modules[i], &status, WNOHANG);
         assert(exited == 0);
      }
      usleep(50000);
   }
   assert(obs_bus_module_available(kinds[0]) && obs_bus_module_available(kinds[1]));
   server_module_tools_adapters_configure();
   char long_name[308] = "vendor:";
   memset(long_name + 7, 'r', 300);
   long_name[307] = 0;
   assert(agent_tools_classification(long_name) == AIMEE_TOOL_CLASS_REMOTE);
   request_context_t owner = {0};
   snprintf(owner.principal, sizeof owner.principal, "%s", "script-fixture");
   snprintf(owner.request_id, sizeof owner.request_id, "%s", "script-fixture-request");
   request_context_set(&owner);
   agent_set_request_vault_principal("webuser:script-fixture");
   session_id_set_override("script-fixture-session");
   test_script_rpc_keeps_permission_scope();
   test_script_rpc_keeps_host_context();
   nested_tool_stop(root);
   policy_and_absence(root);
   puts("script RPC: real tools and execution-policy owner qualification passed");
   return 0;
}

void test_script_rpc_admission_process(const char *executable)
{
   pid_t child = fork();
   assert(child >= 0);
   if (!child)
   {
      execl(executable, executable, "--script-rpc-admission", (char *)NULL);
      _exit(127);
   }
   int status = 0;
   pid_t waited;
   do
   {
      waited = waitpid(child, &status, 0);
   } while (waited < 0 && errno == EINTR);
   if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
      fprintf(stderr, "script admission fixture: status=%d signal=%d\n", status,
              WIFSIGNALED(status) ? WTERMSIG(status) : 0);
   assert(waited == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
}
