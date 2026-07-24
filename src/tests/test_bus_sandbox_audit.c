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

#include "audit_action.h" /* audit_ensure_key */
#include "audit_bus.h"
#include "audit_ledger.h"
#include "cJSON.h"
#include "log.h" /* audit_log_open */
#include "sandbox.h"
#include "server/sandbox_audit_bridge.h"

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

   char home[] = "/tmp/aimee-bussbx-XXXXXX";
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
   const char *secret = "sk-sandbox-DO-NOT-LOG-4b2a";
   char cmd[128];
   snprintf(cmd, sizeof cmd, "APIKEY=%s /usr/bin/true", secret);
   pid_t pid = sandbox_exec(&cfg, cmd, devnull, devnull, NULL);
   assert(pid > 0);
   waitpid(pid, NULL, 0);

   /* A require-isolation exec that is refused. */
   pid_t rc = sandbox_exec_with_readonly(&cfg, "npm publish", devnull, devnull, NULL, NULL, NULL);
   assert(rc == -1);

   audit_bus_stop(); /* drain to the ledger */

   cJSON *rows = audit_ledger_read(NULL, NULL);
   assert(rows);

   /* The fallback row: actor=sandbox, program in command, mode workspace_only+netiso. */
   cJSON *fb = find_row(rows, "sandbox.exec", "true");
   assert(fb);
   assert(strcmp(sval(fb, "actor"), "sandbox") == 0);
   assert(strcmp(sval(fb, "verdict"), "unsandboxed_fallback") == 0);
   assert(strcmp(sval(fb, "mode"), "workspace_only+netiso") == 0);
   assert(sval(fb, "reason_code")[0] != '\0');

   /* The refused row. */
   cJSON *rf = find_row(rows, "sandbox.exec", "npm");
   assert(rf);
   assert(strcmp(sval(rf, "verdict"), "refused") == 0);

   /* THE invariant: the secret in the command's env prefix never reached the ledger. */
   char *dump = cJSON_PrintUnformatted(rows);
   assert(dump);
   assert(!strstr(dump, secret));
   free(dump);

   cJSON_Delete(rows);
   sandbox_set_available_override_for_test(NULL);
   close(devnull);
   printf("test_bus_sandbox_audit: OK (degraded isolation -> bridge -> bus -> ledger; no secret "
          "leak)\n");
   return 0;
}
