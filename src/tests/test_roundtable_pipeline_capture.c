/* test_roundtable_pipeline_capture.c: server-worker result capture seam (P1).
 * Covers result-JSON parsing into an envelope, the submission seam validation
 * (#21), durable finalize into the ledger (#18), the no-op for non-pipeline
 * runs, and the current-attempt guard against late/superseded writes (#30). */

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "roundtable_pipeline.h"
#include "roundtable_pipeline_capture.h"
#include "roundtable_pipeline_eval.h"

int db1_init(const char *path);
void db1_shutdown(void);

static char tmp_db_path[256];

static void setup_db(void)
{
   snprintf(tmp_db_path, sizeof(tmp_db_path), "/tmp/test_rtp_cap_%d.sqlite", (int)getpid());
   unlink(tmp_db_path);
   char p2[300];
   snprintf(p2, sizeof(p2), "%s-wal", tmp_db_path);
   unlink(p2);
   snprintf(p2, sizeof(p2), "%s-shm", tmp_db_path);
   unlink(p2);
   assert(db1_init(tmp_db_path) == 0);
}

static void teardown_db(void)
{
   db1_shutdown();
   unlink(tmp_db_path);
   char p2[300];
   snprintf(p2, sizeof(p2), "%s-wal", tmp_db_path);
   unlink(p2);
   snprintf(p2, sizeof(p2), "%s-shm", tmp_db_path);
   unlink(p2);
}

static void test_parse_review(void)
{
   const char *json =
       "{\"artifact\":\"\",\"converged\":true,\"degraded\":false,\"rounds_run\":2,"
       "\"items_round\":2,\"artifact_round\":2,\"best_round\":2,\"cost_usd\":0.33,"
       "\"items\":[{\"severity\":\"blocking\",\"summary\":\"x\"},"
       "{\"severity\":\"suggestion\"},{\"severity\":\"nit\"},{\"severity\":\"blocking\"}],"
       "\"answered_questions\":[{\"answered\":true},{\"answered\":false}],"
       "\"coverage_gaps\":[\"g1\"]}";
   rtp_envelope_t e;
   rtp_capture_parse(json, 0, &e);
   assert(e.present == 1 && e.parse_ok == 1 && e.has_error == 0);
   assert(e.converged == 1);
   assert(e.item_count == 4);
   assert(e.blocking_count == 2);
   assert(e.suggestion_count == 1);
   assert(e.nit_count == 1);
   assert(e.answered_count == 1);
   assert(e.coverage_gap_count == 1);
   assert(e.cost_known == 1 && e.cost_usd > 0.32 && e.cost_usd < 0.34);
   assert(roundtable_terminal_envelope_valid(&e) == 1);

   /* empty payload -> not present (lost). */
   rtp_capture_parse("", 0, &e);
   assert(e.present == 0);

   /* malformed -> present but parse failed. */
   rtp_capture_parse("{not json", 0, &e);
   assert(e.present == 1 && e.parse_ok == 0);

   /* error payload. */
   rtp_capture_parse("{\"error\":\"boom\"}", 0, &e);
   assert(e.has_error == 1);
   assert(roundtable_terminal_envelope_valid(&e) == 0);
   printf("  parse review: ok\n");
}

static int make_pipeline_pass(int *out_pass_id)
{
   int pid = 0;
   assert(rtp_run_create("idea", NULL, "/r", "testing", &pid) == 0);
   assert(rtp_run_set_state(pid, RTP_STATE_PR_REVIEW, RTP_PHASE_IMPL) == 0);
   int pass_id = 0;
   assert(rtp_pass_create(pid, RTP_PHASE_IMPL, RTP_MODE_REVIEW, 1, "arthash", &pass_id) == 0);
   *out_pass_id = pass_id;
   return pid;
}

static void test_seam_register_and_finalize(void)
{
   teardown_db();
   setup_db();
   int pass_id = 0;
   int pid = make_pipeline_pass(&pass_id);
   (void)pid;

   /* not a pipeline run */
   assert(rtp_seam_register_attempt(0, "oprun_x") == 0);
   /* unknown pass id rejected (#21) */
   assert(rtp_seam_register_attempt(999999, "oprun_x") == -1);
   /* valid */
   assert(rtp_seam_register_attempt(pass_id, "oprun_a") == 1);

   rtp_attempt_t a;
   assert(rtp_attempt_get_by_run("oprun_a", &a) == 0);
   assert(a.is_current == 1);
   assert(strcmp(a.capture_status, RTP_CAP_PENDING) == 0);

   /* finalize for an unrelated run id is a no-op (ordinary ensemble_review). */
   assert(rtp_seam_finalize("oprun_not_pipeline", 1, 0, "{\"converged\":true}") == 0);

   /* finalize the real run with a valid converged review. */
   const char *good = "{\"converged\":true,\"rounds_run\":1,\"items_round\":1,"
                      "\"artifact_round\":1,\"best_round\":1,\"cost_usd\":0.5,\"items\":[]}";
   assert(rtp_seam_finalize("oprun_a", 1, 0, good) == 1);

   assert(rtp_attempt_get_by_run("oprun_a", &a) == 0);
   assert(strcmp(a.capture_status, RTP_CAP_CAPTURED) == 0);
   assert(a.envelope_valid == 1);
   assert(a.cost_known == 1);
   assert(a.result_hash[0] != '\0');

   rtp_pass_t p;
   assert(rtp_pass_get(pass_id, &p) == 0);
   assert(p.envelope_valid == 1);
   assert(p.converged == 1);
   assert(p.blocking_count == 0);
   assert(strcmp(p.status, RTP_PASS_CAPTURED) == 0);
   printf("  seam register + finalize: ok\n");
}

static void test_late_superseded_guard(void)
{
   teardown_db();
   setup_db();
   int pass_id = 0;
   make_pipeline_pass(&pass_id);

   /* attempt 1, then a fresh attempt 2 supersedes it (lost-result re-run). */
   assert(rtp_seam_register_attempt(pass_id, "oprun_old") == 1);
   assert(rtp_seam_register_attempt(pass_id, "oprun_new") == 1);

   rtp_attempt_t old_a;
   assert(rtp_attempt_get_by_run("oprun_old", &old_a) == 0);
   assert(old_a.is_current == 0); /* superseded by the new attempt */

   /* the late terminal from the OLD attempt is recorded but must NOT update the
    * pass aggregate (#30). Give it a blocking finding. */
   const char *late = "{\"converged\":true,\"rounds_run\":1,\"items_round\":1,"
                      "\"artifact_round\":1,\"best_round\":1,\"items\":"
                      "[{\"severity\":\"blocking\",\"summary\":\"stale\"}]}";
   assert(rtp_seam_finalize("oprun_old", 1, 0, late) == 1);

   rtp_attempt_t old_after;
   assert(rtp_attempt_get_by_run("oprun_old", &old_after) == 0);
   assert(old_after.envelope_valid == 1); /* envelope still recorded */

   rtp_pass_t p;
   assert(rtp_pass_get(pass_id, &p) == 0);
   assert(p.blocking_count == 0); /* aggregate untouched by the stale attempt */

   /* now the current attempt finalizes and DOES update the aggregate. */
   const char *cur = "{\"converged\":true,\"rounds_run\":1,\"items_round\":1,"
                     "\"artifact_round\":1,\"best_round\":1,\"items\":"
                     "[{\"severity\":\"blocking\"},{\"severity\":\"blocking\"}]}";
   assert(rtp_seam_finalize("oprun_new", 1, 0, cur) == 1);
   assert(rtp_pass_get(pass_id, &p) == 0);
   assert(p.blocking_count == 2);
   printf("  late/superseded guard: ok\n");
}

static void test_lost_result(void)
{
   teardown_db();
   setup_db();
   int pass_id = 0;
   make_pipeline_pass(&pass_id);
   assert(rtp_seam_register_attempt(pass_id, "oprun_lost") == 1);

   /* empty payload at terminal -> lost_result. */
   assert(rtp_seam_finalize("oprun_lost", 0, 0, "") == 1);
   rtp_attempt_t a;
   assert(rtp_attempt_get_by_run("oprun_lost", &a) == 0);
   assert(a.lost_result == 1);
   assert(strcmp(a.capture_status, RTP_CAP_LOST) == 0);

   rtp_pass_t p;
   assert(rtp_pass_get(pass_id, &p) == 0);
   assert(strcmp(p.status, RTP_PASS_OPEN) == 0); /* awaits re-run, not captured */
   printf("  lost result: ok\n");
}

int main(void)
{
   setup_db();
   test_parse_review();
   test_seam_register_and_finalize();
   test_late_superseded_guard();
   test_lost_result();
   teardown_db();
   printf("test_roundtable_pipeline_capture: all passed\n");
   return 0;
}
