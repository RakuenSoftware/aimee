/* Real Go plans/receipts behind observed and MCP workflow host connections. */
#include "aimee.h"
#include "cJSON.h"
#include "json_fluent.h"
#include "log.h"
#include "platform_test_util.h"
#include "support/module_runtime_fixture.h"
#include <assert.h>
#include <unistd.h>

static char root[512];
static int unavailable, writes, learned;
static const char *receipt = "{\"status\":\"ok\",\"id\":9223372036854775807}";
static const char *expected_rule = "Test command: `make test`";
static const char *expected_workspace;
static double expected_confidence = 0.6;
cJSON *tool_store_workflow(cJSON *args);
int config_workspace_count(void)
{
   return 1;
}
const char *config_workspaces(int index)
{
   assert(index == 0);
   return root;
}
const char *session_id(void)
{
   return "workflow-session";
}
void aimee_log(log_level_t level, const char *module, const char *fmt, ...)
{
   (void)level;
   (void)module;
   (void)fmt;
}
int aimee_module_commands_dispatch_internal(const char *method, const cJSON *args, cJSON **result)
{
   assert(!strcmp(method, "memory.runtime"));
   if (unavailable)
   {
      *result = NULL;
      return -1;
   }
   return module_runtime_fixture_call(args, result);
}
char *kb_v1_action_request(const char *method, cJSON *args)
{
   assert(!strcmp(method, "memory.upsert_workflow"));
   assert(!strcmp(jo_cstr(args, "workspace"), expected_workspace));
   assert(!strcmp(jo_cstr(args, "rule"), expected_rule));
   assert(jo_num(args, "observed_confidence", 0) == expected_confidence);
   assert(!strcmp(jo_cstr(args, "session_id"),
                  expected_confidence == 1 ? "workflow-session" : "post_tool_update"));
   writes++;
   cJSON_Delete(args);
   return receipt ? strdup(receipt) : NULL;
}
void learning_implicit_record_workflow(const char *workspace, const char *signal,
                                       const char *description)
{
   assert(!strcmp(workspace, expected_workspace));
   assert(!strcmp(signal, "test-command"));
   assert(!strcmp(description, expected_rule));
   learned++;
}
int main(void)
{
   char original[4096];
   assert(getcwd(original, sizeof(original)));
   snprintf(root, sizeof(root), "%s/aimee-workflow-XXXXXX", platform_tmpdir());
   assert(mkdtemp(root));
   assert(chdir(root) == 0);
   expected_workspace = strrchr(root, '/') + 1;
   workflow_observe_bash("make test");
   assert(writes == 1 && learned == 1);
   workflow_observe_bash("grep 'make test' README.md");
   assert(writes == 1 && learned == 1);
   const char *bad[] = {NULL, "broken", "{\"status\":\"error\",\"id\":1}", "{\"status\":\"ok\"}",
                        "{\"status\":\"ok\",\"id\":1.5}"};
   for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
   {
      receipt = bad[i];
      workflow_observe_bash("make test");
      assert(learned == 1);
   }
   int before = writes;
   unavailable = 1;
   workflow_observe_bash("make test");
   assert(writes == before && learned == 1);
   unavailable = 0;
   receipt = "{\"status\":\"ok\",\"id\":9223372036854775807}";
   expected_workspace = "explicit";
   expected_confidence = 1;
   expected_rule = "[REDACTED]";
   cJSON *args = cJSON_Parse(
       "{\"project\":\"explicit\",\"signal_type\":\"tests\",\"rule\":\"token=abc\",\"cwd\":\"/"
       "spoof\",\"workspaces\":[\"/"
       "spoof\"],\"mode\":\"observe\",\"operation\":\"workflow-result\",\"session_id\":\"spoof\"}");
   cJSON *result = tool_store_workflow(args);
   cJSON_Delete(args);
   assert(!strcmp(jo_cstr(cJSON_GetArrayItem(result, 0), "text"),
                  "Stored workflow:explicit:tests (memory id 9223372036854775807)"));
   cJSON_Delete(result);
   assert(learned == 1); // Explicit MCP stores did not emit an observation signal before migration.
   args = cJSON_Parse("{\"rule\":\"run tests\"}");
   before = writes;
   result = tool_store_workflow(args);
   cJSON_Delete(args);
   assert(strstr(jo_cstr(cJSON_GetArrayItem(result, 0), "text"), "missing 'rule'"));
   assert(writes == before);
   cJSON_Delete(result);
   assert(chdir(original) == 0);
   assert(rmdir(root) == 0);
   puts("workflow host connections -> Go policy -> KB receipt: ok");
   return 0;
}
