/* test_bus_sandbox_audit.c: the sandbox degraded-isolation audit trail, END TO
 * END through the REAL server bridge (sandbox_audit_bridge.c) onto the audit
 * event bus and into the ledger.
 *
 * test_sandbox.c pins sandbox -> hook (a capturing stub). This pins the other
 * half: the real bridge's field mapping. It installs sandbox_audit_bridge_install
 * + forces the sandbox unavailable (so a guarded exec degrades), drives a real
 * sandbox_exec fallback, drains the async bus, then reads the ledger back and
 * asserts the row's actor/tool/command/mode/verdict — and that NO secret from the
 * command's env-assignment prefix leaked into the trail.
 */
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include <aimee/audit/audit_action.h> /* audit_ensure_key */
#include <aimee/audit/obs_bus.h>
#include <aimee/audit/audit_ledger.h>
#include "cJSON.h"
#include "log.h" /* audit_log_open */
#include "sandbox.h"
#include "server/sandbox_audit_bridge.h"
#include "platform_test_util.h" /* platform_tmpdir: honour TMPDIR, do not leak into /tmp */

static const char *sval(cJSON *row, const char *key)
{
   const char *v = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(row, key));
   return v ? v : "";
}

static cJSON *find_row(cJSON *rows, const char *tool, const char *command)
{
   cJSON *r = NULL;
   cJSON_ArrayForEach(r, rows)
   {
      if (strcmp(sval(r, "tool"), tool) == 0 && strcmp(sval(r, "command"), command) == 0)
         return r;
   }
   return NULL;
}

static int avail_unavailable(const char **reason)
{
   if (reason)
      *reason = "forced-unavailable (test)";
   return 0;
}

int main(void)
{
   printf("test_bus_sandbox_audit:\n");

   char home[256];
   snprintf(home, sizeof home, "%s/aimee-bussbx-XXXXXX", platform_tmpdir());
   if (!mkdtemp(home))
   {
      fprintf(stderr, "FAIL: tmp home\n");
      return 1;
   }
   setenv("AIMEE_HOME", home, 1);
   audit_log_open();
   audit_ensure_key();

   /* Install the REAL bridge + force degradation deterministically. */
   sandbox_audit_bridge_install();
   sandbox_set_available_override_for_test(avail_unavailable);

   int devnull = open("/dev/null", O_WRONLY);
   assert(devnull >= 0);

   sandbox_config_t cfg;
   memset(&cfg, 0, sizeof cfg);
   cfg.mode = SANDBOX_MODE_WORKSPACE_ONLY;
   cfg.network_isolated = 1;

   /* A guarded exec that falls back to unsandboxed (secret in the env prefix). */
   const char *env_secret = "sk-sandbox-env-DO-NOT-LOG-4b2a";
   char cmd[160];
   snprintf(cmd, sizeof cmd, "APIKEY=%s /usr/bin/true", env_secret);
   pid_t pid = sandbox_exec(&cfg, cmd, devnull, devnull, NULL);
   assert(pid > 0);
   waitpid(pid, NULL, 0);

   /* A fallback with network isolation OFF and a secret in the ARGUMENTS: the mode
    * must have no "+netiso" suffix, and the arg secret must not reach the ledger
    * (only the program name "curl" is surfaced). */
   const char *arg_secret = "sk-sandbox-arg-DO-NOT-LOG-9c3e";
   sandbox_config_t noneti;
   memset(&noneti, 0, sizeof noneti);
   noneti.mode = SANDBOX_MODE_ALLOWLIST;
   noneti.network_isolated = 0;
   char cmd2[160];
   snprintf(cmd2, sizeof cmd2, "/usr/bin/curl -H authorization:%s https://x", arg_secret);
   pid = sandbox_exec(&noneti, cmd2, devnull, devnull, NULL);
   assert(pid > 0);
   waitpid(pid, NULL, 0);

   /* A require-isolation exec that is refused. */
   pid_t rc = sandbox_exec_with_readonly(&cfg, "npm publish", devnull, devnull, NULL, NULL, NULL);
   assert(rc == -1);

   obs_bus_stop(); /* drain to the ledger */

   cJSON *rows = audit_ledger_read(NULL, NULL);
   assert(rows);

   /* The fallback row: actor=sandbox, program in command, mode workspace_only+netiso,
    * and the reason_code carries the (test-forced) availability reason verbatim. */
   cJSON *fb = find_row(rows, "sandbox.exec", "true");
   assert(fb);
   assert(strcmp(sval(fb, "actor"), "sandbox") == 0);
   assert(strcmp(sval(fb, "verdict"), "unsandboxed_fallback") == 0);
   assert(strcmp(sval(fb, "mode"), "workspace_only+netiso") == 0);
   assert(strcmp(sval(fb, "reason_code"), "forced-unavailable (test)") == 0);

   /* The network-isolation-off row: mode is "allowlist" with NO "+netiso" suffix. */
   cJSON *nn = find_row(rows, "sandbox.exec", "curl");
   assert(nn);
   assert(strcmp(sval(nn, "mode"), "allowlist") == 0);
   assert(strstr(sval(nn, "mode"), "netiso") == NULL);

   /* The refused row. */
   cJSON *rf = find_row(rows, "sandbox.exec", "npm");
   assert(rf);
   assert(strcmp(sval(rf, "verdict"), "refused") == 0);

   /* THE invariant: NO secret — from the env prefix OR the arguments — ever reached
    * any field of any ledger row. */
   char *dump = cJSON_PrintUnformatted(rows);
   assert(dump);
   assert(!strstr(dump, env_secret));
   assert(!strstr(dump, arg_secret));
   free(dump);

   cJSON_Delete(rows);
   sandbox_set_available_override_for_test(NULL);
   close(devnull);
   printf("test_bus_sandbox_audit: OK (degraded isolation -> bridge -> bus -> ledger; no secret "
          "leak)\n");
   return 0;
}
