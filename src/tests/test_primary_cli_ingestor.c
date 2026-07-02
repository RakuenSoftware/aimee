/* test_primary_cli_ingestor.c -- the external-CLI-primary S2 seam (Slice 2):
 * the ingestor gate is default-off; enforce_preturn routes+binds a session
 * BEFORE send when given a resolved sid + an enforced-routed turn, is dial-gated,
 * and REFUSES to enforce (no-op, returns 0) on a missing session id. Real DB1 +
 * router + engine, stub executors. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "db1.h"
#include "primary_cli_ingestor.h"
#include "wfe_binding.h"
#include "wfe_engine.h"

/* enforced managed workflow "mc" (valid manager shape w/ terminal gate.deliver). */
static const char *WF_MC = "name: mc\n"
                           "enforced: true\n"
                           "start: understand\n"
                           "nodes:\n"
                           "  - id: understand\n"
                           "    block: understand\n"
                           "    next: split\n"
                           "  - id: split\n"
                           "    block: split\n"
                           "    in:\n"
                           "      intent: understand.out\n"
                           "    next: implement\n"
                           "  - id: implement\n"
                           "    block: implement\n"
                           "    in:\n"
                           "      plan: split.out\n"
                           "    next: freeze\n"
                           "  - id: freeze\n"
                           "    block: freeze\n"
                           "    in:\n"
                           "      branch: implement.out\n"
                           "    next: review\n"
                           "  - id: review\n"
                           "    block: review\n"
                           "    in:\n"
                           "      src: freeze.out\n"
                           "    params:\n"
                           "      reviewer: contrarian\n"
                           "    on_pass: rt\n"
                           "    on_fail: split\n"
                           "  - id: rt\n"
                           "    block: gate.roundtable\n"
                           "    in:\n"
                           "      src: freeze.out\n"
                           "    params:\n"
                           "      panel:\n"
                           "        required:\n"
                           "          - security\n"
                           "          - architect\n"
                           "    on_pass: deliver\n"
                           "    on_fail: split\n"
                           "  - id: deliver\n"
                           "    block: gate.deliver\n"
                           "    in:\n"
                           "      verdict: rt.out\n";

static void setup_home(void)
{
   char tmpl[] = "/tmp/pci_home_XXXXXX";
   char *dir = mkdtemp(tmpl);
   assert(dir);
   char wf[512];
   snprintf(wf, sizeof wf, "%s/workflows", dir);
   mkdir(wf, 0755);
   char path[640];
   snprintf(path, sizeof path, "%s/mc.yaml", wf);
   FILE *f = fopen(path, "wb");
   assert(f);
   fputs(WF_MC, f);
   fclose(f);
   setenv("AIMEE_HOME", dir, 1);
   char repo[600];
   snprintf(repo, sizeof repo, "%s/repo", dir);
   mkdir(repo, 0755);
   setenv("AIMEE_WORKFLOW_REPO", repo, 1);
}

static int bound(const char *sid)
{
   char wi[80] = "";
   return db1_wfe_binding_get(sid, wi, sizeof wi, NULL, 0) == 1 && wi[0];
}

int main(void)
{
   printf("primary-cli-ingestor: ");

   setup_home();
   assert(db1_init(":memory:") == 0);
   wfe_reset_block_executors();
   wfe_register_stub_executors();

   /* the gate is default-off and only true for {1,on,true} */
   unsetenv("AIMEE_PRIMARY_CLI_INGESTOR");
   assert(primary_cli_ingestor_enabled() == 0);
   setenv("AIMEE_PRIMARY_CLI_INGESTOR", "1", 1);
   assert(primary_cli_ingestor_enabled() == 1);
   setenv("AIMEE_PRIMARY_CLI_INGESTOR", "on", 1);
   assert(primary_cli_ingestor_enabled() == 1);
   setenv("AIMEE_PRIMARY_CLI_INGESTOR", "true", 1);
   assert(primary_cli_ingestor_enabled() == 1);
   setenv("AIMEE_PRIMARY_CLI_INGESTOR", "0", 1);
   assert(primary_cli_ingestor_enabled() == 0);

   const char *SID = "1a2b3c4d";

   /* dial OFF -> enforce_preturn is inert even with a real sid + enforced route */
   unsetenv("AIMEE_WORKFLOW_ENFORCE_STAGE");
   assert(primary_cli_ingestor_enforce_preturn(SID, "use mc fix the bug", NULL) == 0);
   assert(!bound(SID));

   setenv("AIMEE_WORKFLOW_ENFORCE_STAGE", "advisory", 1);

   /* trust boundary: no resolvable sid -> no-op (0), NEVER a silent bind */
   assert(primary_cli_ingestor_enforce_preturn("", "use mc fix the bug", NULL) == 0);
   assert(primary_cli_ingestor_enforce_preturn(NULL, "use mc fix the bug", NULL) == 0);

   /* an enforced-routed turn WITH a sid -> binds (preventive, before send) */
   assert(primary_cli_ingestor_enforce_preturn(SID, "use mc fix the bug", NULL) == 1);
   assert(bound(SID));

   /* a non-enforced (converse) turn -> stays unbound */
   const char *SID2 = "beefcafe";
   assert(primary_cli_ingestor_enforce_preturn(SID2, "hello there", NULL) == 0);
   assert(!bound(SID2));

   /* posture log: safe (no crash) across all three branches -- off (no-op),
    * on+dial-off (the inert-trap WARN), on+dial-on (active INFO). */
   setenv("AIMEE_PRIMARY_CLI_INGESTOR", "0", 1);
   primary_cli_ingestor_log_posture();
   setenv("AIMEE_PRIMARY_CLI_INGESTOR", "1", 1);
   unsetenv("AIMEE_WORKFLOW_ENFORCE_STAGE");
   primary_cli_ingestor_log_posture();
   setenv("AIMEE_WORKFLOW_ENFORCE_STAGE", "advisory", 1);
   primary_cli_ingestor_log_posture();

   printf("ok\n");
   return 0;
}
