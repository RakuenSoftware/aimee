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
