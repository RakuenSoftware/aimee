/* test_kb_mining.c: KB continuous mining scheduler/jobs. */

#include "artifacts.h"
#include "db2/db2_learning.h"
#include "db2_test_shim.h"
#include "db_postgres.h"
#include "db2_internal.h"
#include "kb_mining.h"
#include "mining.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static void open_db(void)
{
   db2_test_shim_close();
   db2_test_shim_open();
}

static void close_db(void)
{
   db2_test_shim_close();
}

static void exec_sql(const char *sql)
{
   char err[512] = "";
   assert(aimee_pg_exec(db2_conn(), sql, err, sizeof(err)) == 0);
}

static void seed_event(int64_t id, const char *session, const char *type, const char *role,
                       const char *failure_mode, const char *cluster_key)
{
   db2_mining_event_t ev;
   memset(&ev, 0, sizeof(ev));
   ev.source_event_id = id;
   snprintf(ev.session_id, sizeof(ev.session_id), "%s", session ? session : "");
   snprintf(ev.event_type, sizeof(ev.event_type), "%s", type ? type : "");
   snprintf(ev.role, sizeof(ev.role), "%s", role ? role : "");
   snprintf(ev.failure_mode, sizeof(ev.failure_mode), "%s", failure_mode ? failure_mode : "");
   snprintf(ev.payload_json, sizeof(ev.payload_json), "{\"fixture\":true}");
   snprintf(ev.embedding, sizeof(ev.embedding), "[0.1,0.2]");
   snprintf(ev.cluster_key, sizeof(ev.cluster_key), "%s", cluster_key ? cluster_key : "");
   assert(db2_mining_event_upsert(&ev) == 0);
}

static void test_seed_defaults(void)
{
   open_db();
   assert(db2_mining_seed_job_defaults() == 0);

   db2_mining_job_row_t row;
   assert(db2_mining_job_get("pattern_cluster", &row) == 0);
   assert(row.enabled == 1);
   assert(row.interval_s == 900);
   assert(db2_mining_job_get("recurrence", &row) == 0);
   assert(row.enabled == 1);
   assert(row.interval_s == 1800);

   close_db();
   printf("  seed_defaults: ok\n");
}

static void test_recurrence_proposes_workflow_pattern_and_advances_hwm(void)
{
   open_db();
   for (int i = 1; i <= 5; i++)
   {
      char session[32];
      snprintf(session, sizeof(session), "sess-%d", (i % 3) + 1);
      seed_event(i, session, "delegate_exit", "code", "stall/no-writes", "");
   }

   assert(kb_mining_run_once() >= 1);
   assert(db2_artifact_count("workflow_pattern", "proposed") == 1);

   db2_mining_job_row_t row;
   assert(db2_mining_job_get("recurrence", &row) == 0);
   assert(row.hwm == 5);

   close_db();
   printf("  recurrence_proposes_workflow_pattern_and_advances_hwm: ok\n");
}

static void test_pattern_cluster_proposes_interaction_pattern(void)
{
   open_db();
   for (int i = 1; i <= 10; i++)
      seed_event(i, "sess-cluster", "user_correction", "agent", "", "cluster-a");

   assert(kb_mining_run_once() >= 1);
   assert(db2_artifact_count("interaction_pattern", "proposed") == 1);

   db2_mining_job_row_t row;
   assert(db2_mining_job_get("pattern_cluster", &row) == 0);
   assert(row.hwm == 10);

   close_db();
   printf("  pattern_cluster_proposes_interaction_pattern: ok\n");
}

static void test_disabled_job_is_skipped(void)
{
   open_db();
   assert(db2_mining_seed_job_defaults() == 0);
   exec_sql("UPDATE mining_jobs SET enabled = 0 WHERE id = 'recurrence'");
   for (int i = 1; i <= 5; i++)
      seed_event(i, "sess-skip", "delegate_exit", "review", "tool-json-invalid", "");

   assert(kb_mining_run_once() >= 0);
   assert(db2_artifact_count("workflow_pattern", "proposed") == 0);

   db2_mining_job_row_t row;
   assert(db2_mining_job_get("recurrence", &row) == 0);
   assert(row.hwm == 0);

   close_db();
   printf("  disabled_job_is_skipped: ok\n");
}

static void test_job_interval_is_respected(void)
{
   open_db();
   assert(db2_mining_seed_job_defaults() == 0);
   exec_sql("UPDATE mining_jobs SET last_run_at = pg_now_text(), interval_s = 86400"
            " WHERE id = 'recurrence'");
   for (int i = 1; i <= 5; i++)
      seed_event(i, "sess-interval", "delegate_exit", "review", "tool-json-invalid", "");

   assert(kb_mining_run_once() >= 0);
   assert(db2_artifact_count("workflow_pattern", "proposed") == 0);

   db2_mining_job_row_t row;
   assert(db2_mining_job_get("recurrence", &row) == 0);
   assert(row.hwm == 0);

   close_db();
   printf("  job_interval_is_respected: ok\n");
}

/* ingress-compression §4: with kb.mining.failure_learning_enabled on, the
 * recurrence job emits a *pending learning proposal* (sink=artifact) instead of
 * writing the workflow_pattern artifact directly — the artifact appears only after
 * review/Gate-Promote commits the proposal. A repeat cluster corroborates the same
 * pending proposal rather than producing a second one. */
static void test_recurrence_routes_to_learning_when_enabled(void)
{
   /* Enable the flag via an AIMEE_HOME-scoped config the real config_load reads. */
   char home[] = "/tmp/kbmining_fl_XXXXXX";
   assert(mkdtemp(home));
   char yaml[256];
   snprintf(yaml, sizeof(yaml), "%s/aimee.yaml", home);
   FILE *f = fopen(yaml, "w");
   assert(f);
   fputs("kb:\n  mining:\n    failure_learning_enabled: true\n", f);
   fclose(f);
   setenv("AIMEE_HOME", home, 1);

   open_db();
   for (int i = 1; i <= 5; i++)
   {
      char session[32];
      snprintf(session, sizeof(session), "sess-%d", (i % 3) + 1);
      seed_event(i, session, "delegate_exit", "code", "stall/no-writes", "");
   }

   assert(kb_mining_run_once() >= 1);
   /* Routed through learning: NO direct artifact, but a pending proposal exists. */
   assert(db2_artifact_count("workflow_pattern", "proposed") == 0);
   int pid =
       db2_learning_proposal_find_pending("artifact", "delegate_exit:code:stall/no-writes", 0);
   assert(pid > 0);

   /* A second tick on the same cluster corroborates the same proposal (no second
    * proposal, still no artifact). Re-seed a fresh event past the high-watermark. */
   seed_event(6, "sess-1", "delegate_exit", "code", "stall/no-writes", "");
   assert(kb_mining_run_once() >= 0);
   assert(db2_artifact_count("workflow_pattern", "proposed") == 0);
   int pid2 =
       db2_learning_proposal_find_pending("artifact", "delegate_exit:code:stall/no-writes", 0);
   assert(pid2 == pid); /* same pending proposal, corroborated */

   close_db();
   unsetenv("AIMEE_HOME");
   printf("  recurrence_routes_to_learning_when_enabled: ok\n");
}

int main(void)
{
   test_seed_defaults();
   test_recurrence_proposes_workflow_pattern_and_advances_hwm();
   test_pattern_cluster_proposes_interaction_pattern();
   test_disabled_job_is_skipped();
   test_job_interval_is_respected();
   test_recurrence_routes_to_learning_when_enabled();
   printf("kb_mining: all tests passed\n");
   return 0;
}
