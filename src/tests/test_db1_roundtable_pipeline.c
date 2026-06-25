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

   /* list filters. static (off-stack): rtp_run_t embeds a large inline brief[]. */
   static rtp_run_t rows[8];
   int n = rtp_run_list(NULL, rows, 8); /* non-terminal */
   assert(n == 1);                      /* only parked b is non-terminal */
   n = rtp_run_list(RTP_STATE_DONE, rows, 8);
   assert(n == 1);

   /* branch/PR ownership guard (#48): a non-terminal run owns its head_branch. */
   rtp_run_t pb;
   assert(rtp_run_get(b, &pb) == 0);
   snprintf(pb.head_branch, sizeof(pb.head_branch), "feat/shared");
   snprintf(pb.repo_root, sizeof(pb.repo_root), "/r");
   assert(rtp_run_update(&pb) == 0);
   assert(rtp_run_branch_owner("/r", "feat/shared", 0) == b);
   assert(rtp_run_branch_owner("/r", "feat/shared", b) == 0); /* exclude self */
   assert(rtp_run_branch_owner("/r", "feat/other", 0) == 0);
   /* a terminal run does not own its branch. */
   assert(rtp_run_set_state(b, RTP_STATE_ABANDONED, NULL) == 0);
   assert(rtp_run_branch_owner("/r", "feat/shared", 0) == 0);
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

   /* gate-age TTL (#47): a freshly created gate is not over any positive TTL, and
    * hours<=0 (no TTL) never trips. */
   assert(rtp_gate_age_exceeds_hours(pid, 1, 0) == 0); /* TTL disabled */
   assert(rtp_gate_age_exceeds_hours(pid, 1, 1) == 0); /* just created */
   assert(rtp_gate_age_exceeds_hours(pid, 9, 1) == 0); /* no such gate */
   printf("  gates: ok\n");
}

static void test_chunk_group(void)
{
   teardown_db();
   setup_db();
   int pid = 0;
   assert(rtp_run_create("idea", NULL, "/r", "testing", &pid) == 0);
   assert(rtp_pass_max_group(pid, RTP_PHASE_IMPL) == 0);

   /* a chunked review group: 2 chunk members + 1 synthesis member. */
   int group = rtp_pass_max_group(pid, RTP_PHASE_IMPL) + 1;
   int ids[3];
   for (int i = 0; i < 3; i++)
   {
      int chunk_index = (i < 2) ? i : -1; /* -1 = synthesis */
      assert(rtp_pass_create(pid, RTP_PHASE_IMPL, RTP_MODE_REVIEW, i + 1, "h", &ids[i]) == 0);
      rtp_pass_t p;
      assert(rtp_pass_get(ids[i], &p) == 0);
      p.is_chunked = 1;
      p.chunk_group = group;
      p.chunk_index = chunk_index;
      assert(rtp_pass_update(&p) == 0);
   }
   assert(rtp_pass_max_group(pid, RTP_PHASE_IMPL) == 1);

   rtp_group_agg_t agg;
   assert(rtp_pass_group_agg(pid, RTP_PHASE_IMPL, group, &agg) == 0);
   assert(agg.total == 2);
   assert(agg.synthesis_present == 1);
   assert(agg.done == 0 && agg.synthesis_done == 0); /* nothing captured yet */

   /* capture chunk 0 valid, chunk 1 valid+blocking, synthesis valid. */
   for (int i = 0; i < 3; i++)
   {
      rtp_pass_t p;
      assert(rtp_pass_get(ids[i], &p) == 0);
      snprintf(p.status, sizeof(p.status), RTP_PASS_CAPTURED);
      p.envelope_valid = 1;
      if (i == 1)
         p.blocking_count = 2;
      assert(rtp_pass_update(&p) == 0);
   }
   assert(rtp_pass_group_agg(pid, RTP_PHASE_IMPL, group, &agg) == 0);
   assert(agg.total == 2 && agg.done == 2);
   assert(agg.synthesis_done == 1);
   assert(agg.invalid == 0);
   assert(agg.blocking_count == 2);

   /* a synthesis member that omitted required spans blocks the aggregate even
    * when captured+valid (#39). */
   rtp_pass_t synth;
   assert(rtp_pass_get(ids[2], &synth) == 0); /* ids[2] is the synthesis member */
   synth.chunk_omitted = 1;
   assert(rtp_pass_update(&synth) == 0);
   assert(rtp_pass_group_agg(pid, RTP_PHASE_IMPL, group, &agg) == 0);
   assert(agg.invalid == 1);
   assert(agg.synthesis_done == 0);

   /* TWO invalid members must both be counted (#4): a boolean would under-count
    * and leave captured<members forever. Mark both chunk members invalid. */
   for (int i = 0; i < 2; i++)
   {
      rtp_pass_t cp;
      assert(rtp_pass_get(ids[i], &cp) == 0);
      snprintf(cp.status, sizeof(cp.status), RTP_PASS_CAPTURED);
      cp.envelope_valid = 0;
      assert(rtp_pass_update(&cp) == 0);
   }
   assert(rtp_pass_group_agg(pid, RTP_PHASE_IMPL, group, &agg) == 0);
   assert(agg.invalid == 3); /* 2 chunks + the omitted synthesis */
   assert(agg.done == 0);
   /* every member is now terminal (2 invalid chunks + 1 invalid synthesis), so a
    * decision loop sees captured==members and escalates, not "waiting". */
   int members = agg.total + (agg.synthesis_present ? 1 : 0);
   int captured = agg.done + (agg.synthesis_done ? 1 : 0) + agg.invalid;
   assert(captured == members);
   printf("  chunk group aggregate: ok\n");
}

/* #55 exactly-once: rtp_run_cas_state moves the run only when the current state
 * still matches `expected`; a second/stale attempt from the same `expected`
 * loses. This is the primitive that makes a duplicate `gate pass` unable to
 * merge twice. */
static void test_cas_exactly_once(void)
{
   int id = 0;
   assert(rtp_run_create("cas test", RTP_DONEBAR_ZERO_BLOCKING, "/repo", "testing", &id) == 0);
   assert(rtp_run_set_state(id, RTP_STATE_GATE1_PENDING, NULL) == 0);

   /* First claim from GATE1_PENDING -> GATE1_MERGE_PENDING wins. */
   assert(rtp_run_cas_state(id, RTP_STATE_GATE1_PENDING, RTP_STATE_GATE1_MERGE_PENDING) == 0);
   rtp_run_t r;
   assert(rtp_run_get(id, &r) == 0);
   assert(strcmp(r.state, RTP_STATE_GATE1_MERGE_PENDING) == 0);

   /* A concurrent/duplicate caller still expecting GATE1_PENDING loses (no row
    * changed) and must NOT re-trigger the merge. */
   assert(rtp_run_cas_state(id, RTP_STATE_GATE1_PENDING, RTP_STATE_GATE1_MERGE_PENDING) == -1);
   assert(rtp_run_get(id, &r) == 0);
   assert(strcmp(r.state, RTP_STATE_GATE1_MERGE_PENDING) == 0); /* unchanged */

   /* CAS against a non-existent id also fails cleanly. */
   assert(rtp_run_cas_state(999999, RTP_STATE_GATE1_PENDING, RTP_STATE_DONE) == -1);

   /* Fail-path transition (gate -> review_back) is the same primitive: from a
    * fresh gate-pending run, the first claim to proposal_review wins and a stale
    * duplicate loses, so a duplicate `gate fail` cannot re-enter the review loop
    * twice either. */
   int id2 = 0;
   assert(rtp_run_create("cas fail path", RTP_DONEBAR_ZERO_BLOCKING, "/repo", "testing", &id2) ==
          0);
   assert(rtp_run_set_state(id2, RTP_STATE_GATE1_PENDING, NULL) == 0);
   assert(rtp_run_cas_state(id2, RTP_STATE_GATE1_PENDING, RTP_STATE_PROPOSAL_REVIEW) == 0);
   assert(rtp_run_cas_state(id2, RTP_STATE_GATE1_PENDING, RTP_STATE_PROPOSAL_REVIEW) == -1);
   assert(rtp_run_get(id2, &r) == 0);
   assert(strcmp(r.state, RTP_STATE_PROPOSAL_REVIEW) == 0);
   printf("  cas exactly-once: ok\n");
}

int main(void)
{
   setup_db();
   test_run_lifecycle();
   test_cas_exactly_once();
   test_admission();
   test_pass_and_attempts();
   test_gates();
   test_chunk_group();
   teardown_db();
   printf("test_db1_roundtable_pipeline: all passed\n");
   return 0;
}
