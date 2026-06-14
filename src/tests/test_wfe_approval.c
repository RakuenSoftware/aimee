/* test_wfe_approval.c -- W4: HMAC approval signer + gate.human executor. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "db1.h"
#include "wfe_store.h"
#include "wfe_approval.h"
#include "wfe_engine.h"
#include "wfe_iface.h"

static const char *HUM = "name: hum\n"
                         "start: draft\n"
                         "nodes:\n"
                         "  - id: draft\n"
                         "    block: author.proposal\n"
                         "    next: approve\n"
                         "  - id: approve\n"
                         "    block: gate.human\n"
                         "    in:\n"
                         "      src: draft.out\n"
                         "    params:\n"
                         "      policy: interactive\n"
                         "    next: pr\n"
                         "  - id: pr\n"
                         "    block: pr.open\n"
                         "    in:\n"
                         "      src: draft.out\n"
                         "    next: done\n"
                         "  - id: done\n"
                         "    block: merge\n"
                         "    in:\n"
                         "      pr: pr.out\n";

static char g_home[64];

static void setup_home(void)
{
   strcpy(g_home, "/tmp/wfe_appr_XXXXXX");
   char *dir = mkdtemp(g_home);
   assert(dir);
   char wf[128];
   snprintf(wf, sizeof wf, "%s/workflows", dir);
   mkdir(wf, 0755);
   char path[200];
   snprintf(path, sizeof path, "%s/hum.yaml", wf);
   FILE *f = fopen(path, "wb");
   assert(f);
   fputs(HUM, f);
   fclose(f);
   setenv("AIMEE_HOME", dir, 1);
}

int main(void)
{
   printf("wfe-approval: ");
   setup_home();
   assert(db1_init(":memory:") == 0);
   assert(wfe_approval_ensure_key() == 0);

   /* --- key has restrictive perms --- */
   {
      char kp[256];
      wfe_approval_key_path(kp, sizeof kp);
      struct stat st;
      assert(stat(kp, &st) == 0);
      assert((st.st_mode & 0077) == 0); /* no group/other access */
   }

   /* --- sign/verify round-trip + tamper detection --- */
   {
      char sig[65] = "";
      assert(wfe_approval_sign("wi_1", "approve", "hashA", "ts1", sig) == 0);
      assert(wfe_approval_verify("wi_1", "approve", "hashA", "ts1", sig) == 1);
      assert(wfe_approval_verify("wi_1", "approve", "hashB", "ts1", sig) ==
             0); /* artifact changed */
      assert(wfe_approval_verify("wi_2", "approve", "hashA", "ts1", sig) ==
             0); /* wrong work item */
      assert(wfe_approval_verify("wi_1", "approve", "hashA", "ts1", "deadbeef") == 0);
   }

   /* --- a different key cannot forge a valid signature --- */
   {
      char sig[65] = "";
      assert(wfe_approval_sign("wi_1", "approve", "hashA", "ts1", sig) == 0);
      char kp[256];
      wfe_approval_key_path(kp, sizeof kp);
      remove(kp);
      assert(wfe_approval_ensure_key() == 0); /* fresh, different key */
      assert(wfe_approval_verify("wi_1", "approve", "hashA", "ts1", sig) == 0);
   }

   /* --- db1 approval store: present iff matching hash --- */
   {
      assert(db1_work_item_create("wi_s", "r", "p", "hum", "v", "draft", "interactive") == 0);
      assert(wfe_approval_present("wi_s", "approve", "H1") == 0); /* none yet */
      assert(wfe_approval_record("wi_s", "approve", "H1", "user") == 0);
      assert(wfe_approval_present("wi_s", "approve", "H1") == 1);
      assert(wfe_approval_present("wi_s", "approve", "H2") == 0); /* stale: hash changed */
   }

   /* --- gate.human through the engine: pends, then advances after approval --- */
   {
      wfe_reset_block_executors();
      wfe_register_stub_executors();
      wfe_register_human_gate(); /* override gate.human with the real executor */
      char id[80] = "", err[256] = "";
      assert(wfe_work_item_create("hum", "rr", "pp", "interactive", id, err, sizeof err) == 0);
      assert(wfe_engine_run(id, err, sizeof err) == 0);
      db1_work_item_t wi;
      assert(db1_work_item_get(id, &wi) == 1);
      assert(strcmp(wi.current_stage, "approve") == 0);
      assert(strcmp(wi.pause_reason, "pending_human") == 0);

      /* operator approves the current artifact, engine clears pause + resumes */
      assert(wfe_approval_record(id, "approve", wi.content_hash, "user") == 0);
      assert(db1_work_item_clear_pause(id) == 0);
      assert(wfe_engine_run(id, err, sizeof err) == 0);
      assert(db1_work_item_get(id, &wi) == 1);
      assert(strcmp(wi.state, "accepted") == 0);
   }

   printf("ok\n");
   return 0;
}
