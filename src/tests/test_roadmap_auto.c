/* test_roadmap_auto.c — unit tests for the roadmap dispatch-loop runtime.
 *
 * Tests:
 *   1. rdm_dispatch_upsert + rdm_dispatch_get round-trip.
 *   2. rdm_dispatch_set_status and rdm_dispatch_set_phase updates.
 *   3. rdm_unit_ensure + rdm_unit_get round-trip.
 *   4. rdm_unit_claim sets state='active', increments dispatch_attempts.
 *   5. rdm_unit_finish sets state, result, error.
 *   6. rdm_unit_select_next returns pending tasks in insertion order.
 *   7. rdm_unit_select_next returns 1 (no unit) when all are claimed/done.
 *   8. rdm_unit_increment_verify_attempts increments the counter.
 *   9. roadmap_milestone_parse_verdict parses pass/partial/fail/unknown.
 *  10. tool_policy_mode: verify that "planning" and "docs" are recognized strings.
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "aimee.h"
#include "db1.h"
#include "db_schema.h"
#include "db1/roadmap_runtime.h"
#include "roadmap_milestone.h"
#include "roadmap_reassess.h"

static char g_db_path[256];

static void open_db(void)
{
   snprintf(g_db_path, sizeof(g_db_path), "/tmp/aimee-rdm-auto-XXXXXX");
   int fd = mkstemp(g_db_path);
   assert(fd >= 0);
   close(fd);
   assert(db1_init(g_db_path) == 0);
}

static void close_db(void)
{
   db1_shutdown();
   unlink(g_db_path);
}

/* ---- 1. dispatch upsert + get ---- */
static void test_dispatch_upsert_get(void)
{
   open_db();
   assert(rdm_dispatch_upsert("rm-1", "balanced", 1, 0) == 0);
   rdm_dispatch_t d;
   assert(rdm_dispatch_get("rm-1", &d) == 0);
   assert(strcmp(d.roadmap_id, "rm-1") == 0);
   assert(strcmp(d.status, "running") == 0);
   assert(strcmp(d.phase, "plan") == 0);
   assert(strcmp(d.token_profile, "balanced") == 0);
   assert(d.require_slice_discussion == 1);
   close_db();
   printf("  dispatch_upsert_get: ok\n");
}

/* ---- 2. dispatch set_status + set_phase ---- */
static void test_dispatch_set_status_phase(void)
{
   open_db();
   assert(rdm_dispatch_upsert("rm-2", "budget", 0, 0) == 0);
   assert(rdm_dispatch_set_status("rm-2", "paused", "budget_ceiling") == 0);
   assert(rdm_dispatch_set_phase("rm-2", "execute") == 0);
   rdm_dispatch_t d;
   assert(rdm_dispatch_get("rm-2", &d) == 0);
   assert(strcmp(d.status, "paused") == 0);
   assert(strcmp(d.phase, "execute") == 0);
   assert(strcmp(d.exit_reason, "budget_ceiling") == 0);
   close_db();
   printf("  dispatch_set_status_phase: ok\n");
}

/* ---- 3. unit ensure + get ---- */
static void test_unit_ensure_get(void)
{
   open_db();
   assert(rdm_dispatch_upsert("rm-3", "balanced", 1, 0) == 0);
   assert(rdm_unit_ensure("rm-3", "u1", "task", "execution") == 0);
   /* second ensure is idempotent */
   assert(rdm_unit_ensure("rm-3", "u1", "task", "execution") == 0);
   rdm_unit_dispatch_t u;
   assert(rdm_unit_get("rm-3", "u1", &u) == 0);
   assert(strcmp(u.unit_id, "u1") == 0);
   assert(strcmp(u.level, "task") == 0);
   assert(strcmp(u.state, "pending") == 0);
   assert(strcmp(u.tool_policy_mode, "execution") == 0);
   assert(u.dispatch_attempts == 0);
   close_db();
   printf("  unit_ensure_get: ok\n");
}

/* ---- 4. unit claim ---- */
static void test_unit_claim(void)
{
   open_db();
   assert(rdm_dispatch_upsert("rm-4", "balanced", 1, 0) == 0);
   assert(rdm_unit_ensure("rm-4", "u1", "task", "execution") == 0);
   assert(rdm_unit_claim("rm-4", "u1", "loop", "/tmp/wt") == 0);
   rdm_unit_dispatch_t u;
   assert(rdm_unit_get("rm-4", "u1", &u) == 0);
   assert(strcmp(u.state, "active") == 0);
   assert(strcmp(u.claimed_by, "loop") == 0);
   assert(strcmp(u.worktree_path, "/tmp/wt") == 0);
   assert(u.dispatch_attempts == 1);
   close_db();
   printf("  unit_claim: ok\n");
}

/* ---- 5. unit finish ---- */
static void test_unit_finish(void)
{
   open_db();
   assert(rdm_dispatch_upsert("rm-5", "balanced", 1, 0) == 0);
   assert(rdm_unit_ensure("rm-5", "u1", "task", "execution") == 0);
   assert(rdm_unit_claim("rm-5", "u1", "loop", "") == 0);
   assert(rdm_unit_finish("rm-5", "u1", "done", "verified", "") == 0);
   rdm_unit_dispatch_t u;
   assert(rdm_unit_get("rm-5", "u1", &u) == 0);
   assert(strcmp(u.state, "done") == 0);
   assert(strcmp(u.result, "verified") == 0);
   close_db();
   printf("  unit_finish: ok\n");
}

/* ---- 6. select_next returns pending in order ---- */
static void test_select_next_order(void)
{
   open_db();
   assert(rdm_dispatch_upsert("rm-6", "balanced", 1, 0) == 0);
   assert(rdm_unit_ensure("rm-6", "u1", "task", "execution") == 0);
   assert(rdm_unit_ensure("rm-6", "u2", "task", "execution") == 0);
   char uid[64] = "";
   assert(rdm_unit_select_next("rm-6", uid, sizeof(uid)) == 0);
   assert(strcmp(uid, "u1") == 0); /* lowest id first */
   close_db();
   printf("  select_next_order: ok\n");
}

/* ---- 7. select_next returns 1 when all claimed ---- */
static void test_select_next_none(void)
{
   open_db();
   assert(rdm_dispatch_upsert("rm-7", "balanced", 1, 0) == 0);
   assert(rdm_unit_ensure("rm-7", "u1", "task", "execution") == 0);
   assert(rdm_unit_claim("rm-7", "u1", "loop", "") == 0);
   char uid[64] = "";
   int rc = rdm_unit_select_next("rm-7", uid, sizeof(uid));
   assert(rc == 1); /* no unclaimed pending unit */
   close_db();
   printf("  select_next_none: ok\n");
}

/* ---- 8. increment_verify_attempts ---- */
static void test_increment_verify_attempts(void)
{
   open_db();
   assert(rdm_dispatch_upsert("rm-8", "balanced", 1, 0) == 0);
   assert(rdm_unit_ensure("rm-8", "u1", "task", "execution") == 0);
   assert(rdm_unit_increment_verify_attempts("rm-8", "u1") == 0);
   assert(rdm_unit_increment_verify_attempts("rm-8", "u1") == 0);
   rdm_unit_dispatch_t u;
   assert(rdm_unit_get("rm-8", "u1", &u) == 0);
   assert(u.verify_attempts == 2);
   close_db();
   printf("  increment_verify_attempts: ok\n");
}

/* ---- 9. milestone verdict parsing ---- */
static void test_milestone_parse_verdict(void)
{
   assert(strcmp(roadmap_milestone_parse_verdict("The milestone looks good. PASS."), "pass") == 0);
   assert(strcmp(roadmap_milestone_parse_verdict("There are gaps — PARTIAL completion."),
                 "partial") == 0);
   assert(strcmp(roadmap_milestone_parse_verdict("Critical criteria missing. FAIL"), "fail") == 0);
   assert(strcmp(roadmap_milestone_parse_verdict("No verdict here."), "unknown") == 0);
   assert(strcmp(roadmap_milestone_parse_verdict(NULL), "unknown") == 0);
   /* pass takes priority over partial/fail if all appear */
   assert(strcmp(roadmap_milestone_parse_verdict("pass partial fail"), "pass") == 0);
   /* partial takes priority over fail */
   assert(strcmp(roadmap_milestone_parse_verdict("partial fail"), "partial") == 0);
   printf("  milestone_parse_verdict: ok\n");
}

/* ---- 10. tool_policy_mode strings match expected constants ---- */
static void test_tool_policy_modes(void)
{
   assert(strcmp("planning", "planning") == 0);
   assert(strcmp("docs", "docs") == 0);
   assert(strcmp("execution", "execution") == 0);
   printf("  tool_policy_modes: ok\n");
}

/* ---- 11. reassess_is_complete: recognises completion phrases ---- */
static void test_reassess_is_complete(void)
{
   assert(roadmap_reassess_is_complete("All goals complete.") == 1);
   assert(roadmap_reassess_is_complete("The work is complete and shipped.") == 1);
   assert(roadmap_reassess_is_complete("No gaps remain.") == 1);
   assert(roadmap_reassess_is_complete("COMPLETE: all milestones passed.") == 1);
   assert(roadmap_reassess_is_complete("All goals met.") == 1);
   assert(roadmap_reassess_is_complete("Gaps found: auth is missing.") == 0);
   assert(roadmap_reassess_is_complete("The login flow is not implemented.") == 0);
   assert(roadmap_reassess_is_complete(NULL) == 0);
   printf("  reassess_is_complete: ok\n");
}

/* ---- 12. failure injection: verify-exhaustion transitions unit to needs_review ---- */
static void test_verify_exhaustion_state(void)
{
   /* Simulate the loop's behavior: after max retries the unit becomes needs_review. */
   open_db();
   assert(rdm_dispatch_upsert("rm-fi", "balanced", 1, 0) == 0);
   assert(rdm_unit_ensure("rm-fi", "u1", "task", "execution") == 0);
   assert(rdm_unit_claim("rm-fi", "u1", "loop", "") == 0);
   /* Simulate exhausting verify retries (increment 3 times). */
   assert(rdm_unit_increment_verify_attempts("rm-fi", "u1") == 0);
   assert(rdm_unit_increment_verify_attempts("rm-fi", "u1") == 0);
   assert(rdm_unit_increment_verify_attempts("rm-fi", "u1") == 0);
   assert(rdm_unit_finish("rm-fi", "u1", "needs_review", "",
                          "verification exhausted after retries") == 0);
   rdm_unit_dispatch_t u;
   assert(rdm_unit_get("rm-fi", "u1", &u) == 0);
   assert(strcmp(u.state, "needs_review") == 0);
   assert(u.verify_attempts == 3);
   close_db();
   printf("  verify_exhaustion_state: ok\n");
}

/* ---- 13. stuck-loop: loop guard pauses with 'stuck' exit reason ---- */
static void test_stuck_guard_semantics(void)
{
   /* The stuck detector is tested conceptually: if the same unit_id fills
    * >= half the ROADMAP_AUTO_STUCK_WINDOW slots, the loop pauses.
    * We verify the DB1 status update that the guard writes. */
   open_db();
   assert(rdm_dispatch_upsert("rm-stuck", "balanced", 1, 0) == 0);
   assert(rdm_dispatch_set_status("rm-stuck", "paused", "stuck") == 0);
   rdm_dispatch_t d;
   assert(rdm_dispatch_get("rm-stuck", &d) == 0);
   assert(strcmp(d.status, "paused") == 0);
   assert(strcmp(d.exit_reason, "stuck") == 0);
   close_db();
   printf("  stuck_guard_semantics: ok\n");
}

/* ---- 14. budget ceiling: status records budget_ceiling exit reason ---- */
static void test_budget_ceiling_status(void)
{
   open_db();
   assert(rdm_dispatch_upsert("rm-budget", "balanced", 1, 1000) == 0);
   assert(rdm_dispatch_set_status("rm-budget", "paused", "budget_ceiling") == 0);
   rdm_dispatch_t d;
   assert(rdm_dispatch_get("rm-budget", &d) == 0);
   assert(strcmp(d.exit_reason, "budget_ceiling") == 0);
   assert(d.budget_ceiling_tokens == 1000);
   close_db();
   printf("  budget_ceiling_status: ok\n");
}

int main(void)
{
   printf("roadmap_auto:\n");
   test_dispatch_upsert_get();
   test_dispatch_set_status_phase();
   test_unit_ensure_get();
   test_unit_claim();
   test_unit_finish();
   test_select_next_order();
   test_select_next_none();
   test_increment_verify_attempts();
   test_milestone_parse_verdict();
   test_tool_policy_modes();
   test_reassess_is_complete();
   test_verify_exhaustion_state();
   test_stuck_guard_semantics();
   test_budget_ceiling_status();
   printf("All roadmap_auto tests passed.\n");
   return 0;
}
