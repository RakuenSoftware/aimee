/* test_db1_roundtable_pipeline.c: durable ledger for the roundtable authoring
 * pipeline (P0). Exercises runs/passes/attempts/gates CRUD, the state machine,
 * admission counting, attempt accounting (#23/#25), and the current-attempt
 * supersede guard (#30). */

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "roundtable_pipeline.h"

int db1_init(const char *path);
void db1_shutdown(void);

static char tmp_db_path[256];

static void rm_aux(const char *base)
{
   char p2[300];
   snprintf(p2, sizeof(p2), "%s-wal", base);
   unlink(p2);
   snprintf(p2, sizeof(p2), "%s-shm", base);
   unlink(p2);
}

static void setup_db(void)
{
   snprintf(tmp_db_path, sizeof(tmp_db_path), "/tmp/test_db1_rtp_%d.sqlite", (int)getpid());
   unlink(tmp_db_path);
   rm_aux(tmp_db_path);
   assert(db1_init(tmp_db_path) == 0);
}

static void teardown_db(void)
{
   db1_shutdown();
   unlink(tmp_db_path);
   rm_aux(tmp_db_path);
}

static void test_run_lifecycle(void)
{
   int id = 0;
   assert(rtp_run_create("add a foo widget", RTP_DONEBAR_ZERO_BLOCKING, "/repo", "testing", &id) ==
          0);
   assert(id > 0);

   rtp_run_t r;
   assert(rtp_run_get(id, &r) == 0);
   assert(r.id == id);
   assert(strcmp(r.state, RTP_STATE_DRAFTING) == 0);
   assert(strcmp(r.phase, RTP_PHASE_PROPOSAL) == 0);
   assert(strcmp(r.admission_class, RTP_ADMIT_ACTIVE) == 0);
   assert(strcmp(r.done_bar, RTP_DONEBAR_ZERO_BLOCKING) == 0);
   assert(strcmp(r.base_branch, "testing") == 0);
   assert(strcmp(r.idea, "add a foo widget") == 0);

   /* default cost scope is roundtable-only until usage_ledger lands (#49). */
   assert(strcmp(r.cost_scope, "roundtable_only") == 0);

   /* full update round-trip */
   snprintf(r.head_branch, sizeof(r.head_branch), "feat/foo");
   snprintf(r.proposal_ref, sizeof(r.proposal_ref), "docs/proposals/pending/foo.md");
   snprintf(r.proposal_origin_hash, sizeof(r.proposal_origin_hash), "deadbeef");
   r.proposal_pr_number = 42;
   r.total_cost_usd = 1.25;
   assert(rtp_run_update(&r) == 0);

   rtp_run_t r2;
   assert(rtp_run_get(id, &r2) == 0);
   assert(strcmp(r2.head_branch, "feat/foo") == 0);
   assert(strcmp(r2.proposal_origin_hash, "deadbeef") == 0);
   assert(r2.proposal_pr_number == 42);
   assert(r2.total_cost_usd > 1.24 && r2.total_cost_usd < 1.26);

   /* state transitions */
   assert(rtp_run_set_state(id, RTP_STATE_PROPOSAL_REVIEW, RTP_PHASE_PROPOSAL) == 0);
   assert(rtp_run_get(id, &r2) == 0);
   assert(strcmp(r2.state, RTP_STATE_PROPOSAL_REVIEW) == 0);

   /* phase=NULL keeps phase */
   assert(rtp_run_set_state(id, RTP_STATE_GATE1_PENDING, NULL) == 0);
   assert(rtp_run_get(id, &r2) == 0);
   assert(strcmp(r2.state, RTP_STATE_GATE1_PENDING) == 0);
   assert(strcmp(r2.phase, RTP_PHASE_PROPOSAL) == 0);
   printf("  run lifecycle: ok\n");
}

static void test_admission(void)
{
   /* one active to begin (from previous test's run still non-terminal) — count
    * by creating a fresh DB instead to keep this deterministic. */
   teardown_db();
   setup_db();

   assert(rtp_run_count_active() == 0);
   int a = 0, b = 0;
   assert(rtp_run_create("idea A", NULL, "/r", "testing", &a) == 0);
   assert(rtp_run_count_active() == 1);
   assert(rtp_run_create("idea B", NULL, "/r", "testing", &b) == 0);
   assert(rtp_run_count_active() == 2);

   /* parking releases the active slot (#48). */
   rtp_run_t r;
   assert(rtp_run_get(b, &r) == 0);
   snprintf(r.admission_class, sizeof(r.admission_class), RTP_ADMIT_PARKED);
   assert(rtp_run_update(&r) == 0);
   assert(rtp_run_count_active() == 1);

   /* terminal states are not active. */
   assert(rtp_run_set_state(a, RTP_STATE_DONE, NULL) == 0);
   assert(rtp_run_count_active() == 0);

   /* list filters */
   rtp_run_t rows[8];
   int n = rtp_run_list(NULL, rows, 8); /* non-terminal */
   assert(n == 1);                      /* only parked b is non-terminal */
   n = rtp_run_list(RTP_STATE_DONE, rows, 8);
   assert(n == 1);
   printf("  admission + list: ok\n");
}

static void test_pass_and_attempts(void)
{
   teardown_db();
   setup_db();
   int pid = 0;
   assert(rtp_run_create("idea", NULL, "/r", "testing", &pid) == 0);

   assert(rtp_pass_max_no(pid, RTP_PHASE_IMPL) == 0);
   int pass_no = rtp_pass_max_no(pid, RTP_PHASE_IMPL) + 1;
   int pass_id = 0;
   assert(rtp_pass_create(pid, RTP_PHASE_IMPL, RTP_MODE_REVIEW, pass_no, "h1", &pass_id) == 0);
   assert(pass_id > 0);
   assert(rtp_pass_max_no(pid, RTP_PHASE_IMPL) == 1);

   /* attempt 1: submitted, then lost_result */
   int att1 = 0;
   assert(rtp_attempt_create(pass_id, 1, "oprun_1", &att1) == 0);
   rtp_attempt_t a;
   assert(rtp_attempt_get_by_run("oprun_1", &a) == 0);
   assert(a.id == att1);
   assert(a.is_current == 1);
   assert(strcmp(a.capture_status, RTP_CAP_PENDING) == 0);

   snprintf(a.capture_status, sizeof(a.capture_status), RTP_CAP_LOST);
   a.lost_result = 1;
   snprintf(a.terminal_at, sizeof(a.terminal_at), "2026-06-11T00:00:00Z");
   assert(rtp_attempt_update(&a) == 0);

   /* attempt 2 under same pass id (lost-result re-run, #19); supersede attempt 1 */
   assert(rtp_attempt_max_no(pass_id) == 1);
   int att2 = 0;
   assert(rtp_attempt_create(pass_id, 2, "oprun_2", &att2) == 0);
   assert(rtp_attempt_supersede_others(pass_id, att2) == 0);

   rtp_attempt_t cur;
   assert(rtp_attempt_current(pass_id, &cur) == 0);
   assert(cur.id == att2);
   assert(cur.attempt_no == 2);

   /* attempt 1 retained as history but not current (#25/#30) */
   assert(rtp_attempt_get_by_run("oprun_1", &a) == 0);
   assert(a.is_current == 0);
   assert(a.lost_result == 1); /* not overwritten */

   /* finalize attempt 2 successfully and record aggregate on the pass */
   snprintf(cur.capture_status, sizeof(cur.capture_status), RTP_CAP_CAPTURED);
   snprintf(cur.terminal_status, sizeof(cur.terminal_status), "completed");
   snprintf(cur.parse_status, sizeof(cur.parse_status), "ok");
   cur.envelope_valid = 1;
   cur.cost_usd = 0.42;
   cur.cost_known = 1;
   assert(rtp_attempt_update(&cur) == 0);

   rtp_pass_t p;
   assert(rtp_pass_get(pass_id, &p) == 0);
   p.envelope_valid = 1;
   p.converged = 1;
   p.blocking_count = 0;
   p.cost_usd = 0.42;
   snprintf(p.status, sizeof(p.status), RTP_PASS_DONE);
   assert(rtp_pass_update(&p) == 0);

   rtp_pass_t p2;
   assert(rtp_pass_latest(pid, RTP_PHASE_IMPL, &p2) == 0);
   assert(p2.id == pass_id);
   assert(p2.envelope_valid == 1);
   assert(strcmp(p2.status, RTP_PASS_DONE) == 0);
   printf("  passes + attempts: ok\n");
}

static void test_gates(void)
{
   teardown_db();
   setup_db();
   int pid = 0;
   assert(rtp_run_create("idea", NULL, "/r", "testing", &pid) == 0);

   int gid = 0;
   assert(rtp_gate_create(pid, 1, 7, "abc123", &gid) == 0);
   rtp_gate_t g;
   assert(rtp_gate_get(pid, 1, &g) == 0);
   assert(g.id == gid);
   assert(g.pr_number == 7);
   assert(strcmp(g.expected_head_sha, "abc123") == 0);
   assert(g.verdict[0] == '\0');

   /* invalid gate number rejected */
   assert(rtp_gate_create(pid, 3, 0, "", NULL) == -1);

   /* resolve pass + merge */
   snprintf(g.verdict, sizeof(g.verdict), "pass");
   snprintf(g.actor, sizeof(g.actor), "operator");
   snprintf(g.merge_sha, sizeof(g.merge_sha), "mergesha1");
   snprintf(g.merge_executor, sizeof(g.merge_executor), "git_pr");
   snprintf(g.resolved_at, sizeof(g.resolved_at), "2026-06-11T01:00:00Z");
   assert(rtp_gate_update(&g) == 0);

   rtp_gate_t g2;
   assert(rtp_gate_get(pid, 1, &g2) == 0);
   assert(strcmp(g2.verdict, "pass") == 0);
   assert(strcmp(g2.merge_sha, "mergesha1") == 0);
   assert(strcmp(g2.merge_executor, "git_pr") == 0);
   printf("  gates: ok\n");
}

int main(void)
{
   setup_db();
   test_run_lifecycle();
   test_admission();
   test_pass_and_attempts();
   test_gates();
   teardown_db();
   printf("test_db1_roundtable_pipeline: all passed\n");
   return 0;
}
