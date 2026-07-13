/* test_trigger_e2e.c — end-to-end: a proposal committed under
 * docs/proposals/pending/ in a REAL git repo is picked up by the proposals
 * trigger (real git subprocesses, real materialization, real DB1 work-item
 * store, real cost-cap stamping), filed as an autonomous work item bound to
 * that repo, and driven to a TERMINAL state by the real autonomy scheduler +
 * engine. Only the block executors are stubbed (the vtable seam every engine
 * test uses) — everything between "a proposal appears in pending/" and
 * "the workflow completed" is the production code path. */
#include "wfe_test_home.h"
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "cJSON.h"
#include "config.h"
#include "db1.h"
#include "log.h"
#include "wfe_engine.h"
#include "wfe_scheduler.h"
#include "wfe_store.h"

/* ---- stubs for trigger_scheduler.c symbols outside the proposals path ---- */

int config_load(config_t *cfg)
{
   memset(cfg, 0, sizeof(*cfg));
   return 0;
}

#include "skill_curator.h"
int skill_curator_maybe(void)
{
   return 0;
}

#include "db1_trigger.h"
int db1_trigger_insert(const char *id, const char *source, const char *event, const char *task,
                       const char *workspace, const char *metadata)
{
   (void)id;
   (void)source;
   (void)event;
   (void)task;
   (void)workspace;
   (void)metadata;
   return 0;
}
int db1_trigger_status_set(const char *id, const char *status, const char *pipeline_id,
                           const char *error)
{
   (void)id;
   (void)status;
   (void)pipeline_id;
   (void)error;
   return 0;
}

#include "pipelines.h"
int db1_pipeline_create(const char *task, const char *request_classification,
                        const char *plan_depth, int *out_id)
{
   (void)task;
   (void)request_classification;
   (void)plan_depth;
   if (out_id)
      *out_id = 1;
   return 0;
}
int db1_pipeline_cancel(int pipeline_id)
{
   (void)pipeline_id;
   return 0;
}

int platform_random_bytes(void *buf, size_t len)
{
   memset(buf, 0x42, len);
   return 0;
}

#include "db1/db1_cron_jobs.h"
int db1_cron_jobs_load(cron_job_t *out, int max, int enabled_only)
{
   (void)out;
   (void)max;
   (void)enabled_only;
   return 0;
}

int cron_run_config_job(const cron_job_t *job, cJSON **out_resp)
{
   (void)job;
   (void)out_resp;
   return 0;
}

/* Pull in the REAL scan_proposals (static) with the real git capture path. */
#include "../server/trigger_scheduler.c"

/* ---- helpers ---- */

static int sh(const char *fmt, ...)
{
   char cmd[2048];
   va_list ap;
   va_start(ap, fmt);
   vsnprintf(cmd, sizeof cmd, fmt, ap);
   va_end(ap);
   return system(cmd);
}

/* A two-step workflow: normalize the proposal, then author the plan. Terminal
 * after `plan` advances (no next edge). */
static const char *WF = "name: e2e\n"
                        "start: draft\n"
                        "nodes:\n"
                        "  - id: draft\n"
                        "    block: author.proposal\n"
                        "    next: plan\n"
                        "    on_fail: draft\n"
                        "  - id: plan\n"
                        "    block: author.plan\n"
                        "    in:\n"
                        "      proposal: draft.out\n";

int main(void)
{
   printf("trigger-e2e: ");

   /* AIMEE_HOME with the workflow definition. */
   char home[] = "/tmp/wfe_te_home_XXXXXX";
   assert(wfe_test_mkdtemp(home));
   char path[512];
   snprintf(path, sizeof path, "%s/workflows", home);
   mkdir(path, 0755);
   snprintf(path, sizeof path, "%s/workflows/e2e.yaml", home);
   FILE *f = fopen(path, "wb");
   assert(f);
   fputs(WF, f);
   fclose(f);
   setenv("AIMEE_HOME", home, 1);
   unsetenv("AIMEE_AUTONOMY_MAX_USD"); /* assert the default $5 cap below */
   assert(db1_init(":memory:") == 0);

   /* A real git repo with a pending proposal committed on its default branch. */
   char repo[] = "/tmp/wfe_te_repo_XXXXXX";
   assert(wfe_test_mkdtemp(repo));
   assert(sh("cd %s && git init -q && git config user.email t@t && git config user.name t && "
             "git config commit.gpgsign false && mkdir -p docs/proposals/pending && "
             "printf '# Add /healthz\\n\\nReturn {status:ok}.\\n' > "
             "docs/proposals/pending/add-healthz.md && "
             "git add -A && git commit -qm 'proposal: add /healthz'",
             repo) == 0);

   trigger_rule_t rule;
   memset(&rule, 0, sizeof rule);
   snprintf(rule.source, sizeof rule.source, "proposals");
   snprintf(rule.workspace, sizeof rule.workspace, "%s", repo);
   snprintf(rule.pipeline_template, sizeof rule.pipeline_template, "e2e");
   /* event/schedule/mode empty: default dir, auto-detected ref, autonomous. */

   /* --- the trigger picks the proposal up --- */
   scan_proposals(&rule, 2);

   db1_work_item_t *items = NULL;
   int n = db1_work_item_list(&items);
   assert(n == 1);
   assert(strcmp(items[0].state, "active") == 0);
   assert(strcmp(items[0].mode, "autonomous") == 0);
   assert(strcmp(items[0].repo, repo) == 0);            /* bound to the watched repo */
   assert(strcmp(items[0].workflow_name, "e2e") == 0);  /* on the rule's workflow */
   assert(items[0].work_item_max_cost_usd == 5.0);      /* default USD ceiling stamped */
   /* The proposal was materialized under $AIMEE_HOME/triggers/proposals/. */
   char want_prefix[600];
   snprintf(want_prefix, sizeof want_prefix, "%s/triggers/proposals/", home);
   assert(strncmp(items[0].proposal_path, want_prefix, strlen(want_prefix)) == 0);
   f = fopen(items[0].proposal_path, "rb");
   assert(f);
   char body[256] = {0};
   size_t rd = fread(body, 1, sizeof body - 1, f);
   fclose(f);
   assert(rd > 0 && strstr(body, "# Add /healthz") == body);
   char id[80];
   snprintf(id, sizeof id, "%s", items[0].work_item_id);
   free(items);

   /* --- the autonomy scheduler completes the workflow --- */
   wfe_reset_block_executors();
   wfe_register_stub_executors();
   wfe_scheduler_run_once();

   db1_work_item_t done;
   assert(db1_work_item_get(id, &done) == 1);
   assert(strcmp(done.state, "accepted") == 0); /* ran draft -> plan -> terminal */

   /* The audit log shows the full run: create, draft->plan advance, terminal. */
   db1_lifecycle_event_t *evs = NULL;
   int nev = db1_lifecycle_event_list(id, &evs);
   assert(nev >= 3);
   int advances = 0, terminals = 0;
   for (int i = 0; i < nev; i++)
   {
      if (strcmp(evs[i].kind, "advance") == 0)
         advances++;
      if (strcmp(evs[i].kind, "terminal") == 0)
         terminals++;
   }
   free(evs);
   assert(advances >= 1 && terminals == 1);

   /* --- a re-scan of the same tree files nothing (dedup), including for the
    *     now-completed proposal: done work is never re-run --- */
   scan_proposals(&rule, 2);
   items = NULL;
   assert(db1_work_item_list(&items) == 1);
   free(items);

   /* --- a NEW proposal on the branch files a second run --- */
   assert(sh("cd %s && printf '# Second\\n' > docs/proposals/pending/second.md && "
             "git add -A && git commit -qm 'proposal: second'",
             repo) == 0);
   scan_proposals(&rule, 2);
   items = NULL;
   n = db1_work_item_list(&items);
   assert(n == 2);
   free(items);
   wfe_scheduler_run_once();
   items = NULL;
   n = db1_work_item_list(&items);
   assert(n == 2);
   for (int i = 0; i < n; i++)
      assert(strcmp(items[i].state, "accepted") == 0);
   free(items);

   printf("ok (proposal -> trigger -> autonomous run -> accepted, x2)\n");
   return 0;
}
