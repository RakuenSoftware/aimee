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

static void seed_attempt(int64_t id, const char *session, const char *scope_id,
                         const char *task_family, const char *failure_mode, const char *outcome,
                         const char *recovery_action)
{
   db2_mining_event_t ev;
   memset(&ev, 0, sizeof(ev));
   ev.source_event_id = id;
   snprintf(ev.session_id, sizeof(ev.session_id), "%s", session);
   snprintf(ev.event_type, sizeof(ev.event_type), "task_attempt");
   snprintf(ev.role, sizeof(ev.role), "code");
   snprintf(ev.failure_mode, sizeof(ev.failure_mode), "%s", failure_mode);
   snprintf(ev.scope_kind, sizeof(ev.scope_kind), "workspace");
   snprintf(ev.scope_id, sizeof(ev.scope_id), "%s", scope_id);
   snprintf(ev.task_family, sizeof(ev.task_family), "%s", task_family);
   snprintf(ev.action_sequence, sizeof(ev.action_sequence), "inspect,run,repair");
   snprintf(ev.error_signature, sizeof(ev.error_signature), "exit:17");
   snprintf(ev.environment, sizeof(ev.environment), "linux:test");
   snprintf(ev.preconditions, sizeof(ev.preconditions), "service-ready");
   snprintf(ev.outcome, sizeof(ev.outcome), "%s", outcome);
   snprintf(ev.recovery_action, sizeof(ev.recovery_action), "%s", recovery_action);
   snprintf(ev.payload_json, sizeof(ev.payload_json), "{\"complete_attempt\":true}");
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

static void test_recurrence_materializes_observation_and_advances_hwm(void)
{
   open_db();
   for (int i = 1; i <= 5; i++)
   {
      char session[32];
      snprintf(session, sizeof(session), "sess-%d", (i % 3) + 1);
      seed_event(i, session, "delegate_exit", "code", "stall/no-writes", "");
   }

   assert(kb_mining_run_once() >= 1);
   assert(db2_artifact_count("workflow_pattern", "proposed") == 0);
   learning_observation_t observations[2];
   int observation_n = db2_learning_observation_list("active", "workspace", "", 2, observations, 2);
   assert(observation_n == 1);
   assert(strcmp(observations[0].observation_type, "recurring_failure") == 0);
   assert(observations[0].evidence_count == 5);
   assert(observations[0].independent_session_count == 3);
   int64_t evidence_ids[8];
   assert(db2_learning_observation_evidence_ids(observations[0].observation_id, evidence_ids, 8) ==
          5);

   db2_mining_job_row_t row;
   assert(db2_mining_job_get("recurrence", &row) == 0);
   assert(row.hwm == 5);

   /* Source erasure cascades through evidence links; the next deterministic
    * reconciliation retires the observation below its support floor. */
   exec_sql("DELETE FROM interaction_event_embeddings WHERE source_event_id IN (1,2,3)");
   exec_sql("UPDATE mining_jobs SET last_run_at='' WHERE id='recurrence'");
   assert(kb_mining_run_once() >= 1);
   learning_observation_t retired;
   assert(db2_learning_observation_get(observations[0].observation_id, &retired) == 0);
   assert(strcmp(retired.status, "retired") == 0 && retired.evidence_count == 2);
   assert(retired.retired_at[0] != '\0');

   close_db();
   printf("  recurrence_materializes_observation_and_advances_hwm: ok\n");
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
   learning_proposal_t proposal;
   assert(db2_learning_proposal_get(pid, &proposal) == 0);
   assert(strstr(proposal.evidence_refs, "learning_observation") != NULL);
   assert(strstr(proposal.evidence_refs, "interaction_event") != NULL);

   /* A second qualifying window on the same cluster corroborates and refreshes
    * the exact evidence manifest on the same proposal. */
   seed_event(6, "sess-1", "delegate_exit", "code", "stall/no-writes", "");
   seed_event(7, "sess-4", "delegate_exit", "code", "stall/no-writes", "");
   seed_event(8, "sess-5", "delegate_exit", "code", "stall/no-writes", "");
   exec_sql("UPDATE mining_jobs SET last_run_at='' WHERE id='recurrence'");
   assert(kb_mining_run_once() >= 0);
   assert(db2_artifact_count("workflow_pattern", "proposed") == 0);
   int pid2 =
       db2_learning_proposal_find_pending("artifact", "delegate_exit:code:stall/no-writes", 0);
   assert(pid2 == pid); /* same pending proposal, corroborated */
   assert(db2_learning_proposal_get(pid, &proposal) == 0);
   assert(proposal.corroboration_count == 2);
   assert(strstr(proposal.evidence_refs, "\"stable_id\":8") != NULL);

   close_db();
   unsetenv("AIMEE_HOME");
   printf("  recurrence_routes_to_learning_when_enabled: ok\n");
}

static void test_recovery_observation_and_scope_partition(void)
{
   open_db();
   seed_attempt(1, "s1", "ws-a", "build", "environment/service", "success",
                "start dependency then retry");
   seed_attempt(2, "s2", "ws-a", "build", "environment/service", "success",
                "start dependency then retry");
   /* Same signature in another visibility partition cannot corroborate ws-a. */
   seed_attempt(3, "s3", "ws-b", "build", "environment/service", "success",
                "start dependency then retry");
   assert(kb_mining_run_once() >= 1);
   learning_observation_t rows[8];
   int n = db2_learning_observation_list("active", "workspace", "ws-a", 8, rows, 8);
   assert(n == 1);
   assert(strcmp(rows[0].observation_type, "successful_recovery") == 0);
   assert(rows[0].evidence_count == 2);
   assert(db2_learning_observation_list("active", "workspace", "ws-b", 8, rows, 8) == 0);
   close_db();
   printf("  recovery_observation_and_scope_partition: ok\n");
}

static void test_application_attribution_contract(void)
{
   open_db();
   seed_attempt(10, "apply-s1", "ws-a", "deploy", "tool/failure", "failure", "");
   learning_application_event_t app;
   memset(&app, 0, sizeof(app));
   snprintf(app.application_id, sizeof(app.application_id), "application-10");
   app.source_event_id = 10;
   snprintf(app.session_id, sizeof(app.session_id), "apply-s1");
   snprintf(app.scope_kind, sizeof(app.scope_kind), "workspace");
   snprintf(app.scope_id, sizeof(app.scope_id), "ws-a");
   snprintf(app.task_family, sizeof(app.task_family), "deploy");
   snprintf(app.procedure_artifact_id, sizeof(app.procedure_artifact_id), "procedure-1");
   snprintf(app.outcome, sizeof(app.outcome), "failure");
   app.applied = 1;
   assert(db2_learning_application_record(&app) != 0); /* exposure is not application */
   app.retrieved = app.rendered = app.selected = 1;
   snprintf(app.retrieved_refs, sizeof(app.retrieved_refs), "[\"procedure-1\"]");
   snprintf(app.rendered_refs, sizeof(app.rendered_refs), "[\"procedure-1\"]");
   snprintf(app.selected_refs, sizeof(app.selected_refs), "[\"procedure-1\"]");
   snprintf(app.applied_refs, sizeof(app.applied_refs), "[\"procedure-1\"]");
   assert(db2_learning_application_record(&app) == 0);
   learning_application_event_t loaded;
   assert(db2_learning_application_get("application-10", &loaded) == 0);
   assert(loaded.applied == 1 && loaded.selected == 1);
   assert(strcmp(loaded.outcome, "failure") == 0);

   learning_observation_evidence_input_t evidence = {10, "", "contradicts"};
   assert(db2_learning_observation_refresh(
              "unstable-test", "workspace", "ws-a", "unstable_procedure", "Unstable procedure",
              "Applied procedure failed", "test-v1", &evidence, 1, "") == 0);
   learning_observation_t obs;
   assert(db2_learning_observation_get("unstable-test", &obs) == 0);
   assert(strcmp(obs.status, "active") == 0);
   /* Evidence from another scope is rejected, rather than widening visibility. */
   seed_attempt(11, "apply-s2", "ws-b", "deploy", "tool/failure", "failure", "");
   evidence.source_event_id = 11;
   assert(db2_learning_observation_refresh(
              "unstable-test", "workspace", "ws-a", "unstable_procedure", "Unstable procedure",
              "Applied procedure failed", "test-v1", &evidence, 1, "") != 0);
   close_db();
   printf("  application_attribution_contract: ok\n");
}

int main(void)
{
   test_seed_defaults();
   test_recurrence_materializes_observation_and_advances_hwm();
   test_pattern_cluster_proposes_interaction_pattern();
   test_disabled_job_is_skipped();
   test_job_interval_is_respected();
   test_recurrence_routes_to_learning_when_enabled();
   test_recovery_observation_and_scope_partition();
   test_application_attribution_contract();
   printf("kb_mining: all tests passed\n");
   return 0;
}
