/* test_decision_log.c: the S5 governance decision write path + the
 * one-active-per-scope invariant, exercised against the DB2 sqlite shim. */
#include "../db2/db2_test_shim.h"
#include "decision_log.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

/* A recorded decision is active and round-trips its governance fields. */
static void test_record_is_active(void)
{
   db2_decision_log_row_t row;
   int rc = db2_decision_log_record("deploy-window", "opt-a|opt-b", "opt-a", "lower risk",
                                    "jbailes", 7, "2026-09-01", 0, &row);
   assert(rc == 0);
   assert(strcmp(row.status, "active") == 0);
   assert(strcmp(row.subject, "deploy-window") == 0);
   assert(strcmp(row.chosen, "opt-a") == 0);
   assert(strcmp(row.author, "jbailes") == 0);
   assert(strcmp(row.revisit_when, "2026-09-01") == 0);
   assert(row.linked_policy_id == 7);
   assert(row.id > 0);
   printf("  PASS: test_record_is_active\n");
}

/* Superseding flips the prior decision to 'superseded' and the new one is active. */
static void test_supersede_flips_prior(void)
{
   db2_decision_log_row_t a, b, a_after;
   assert(db2_decision_log_record("retention", "keep|drop", "keep", "v1", "op", 0, "", 0, &a) == 0);
   /* b supersedes a — same scope (subject=retention, policy=0) is fine because a
    * is flipped to superseded in the same txn. */
   assert(db2_decision_log_record("retention", "keep|drop", "drop", "v2", "op", 0, "", a.id, &b) ==
          0);
   assert(strcmp(b.status, "active") == 0);
   assert(db2_decision_log_get(a.id, &a_after) == 0);
   assert(strcmp(a_after.status, "superseded") == 0);
   printf("  PASS: test_supersede_flips_prior\n");
}

/* A second active decision for the SAME (subject, linked_policy_id) is rejected
 * by the invariant, and the first stays active (transaction rolled back). */
static void test_one_active_per_scope(void)
{
   db2_decision_log_row_t first, first_after;
   assert(db2_decision_log_record("budget", "a|b", "a", "r", "op", 42, "", 0, &first) == 0);
   int rc = db2_decision_log_record("budget", "a|b", "b", "r", "op", 42, "", 0, NULL);
   assert(rc != 0); /* rejected: an active decision already exists for this scope */
   assert(db2_decision_log_get(first.id, &first_after) == 0);
   assert(strcmp(first_after.status, "active") == 0); /* first untouched */
   printf("  PASS: test_one_active_per_scope\n");
}

/* Different scopes may both be active concurrently. */
static void test_distinct_scopes_coexist(void)
{
   db2_decision_log_row_t x, y;
   assert(db2_decision_log_record("scope-x", "a", "a", "r", "op", 0, "", 0, &x) == 0);
   assert(db2_decision_log_record("scope-y", "a", "a", "r", "op", 0, "", 0, &y) == 0);
   /* same subject, different linked_policy_id -> different scope, both active */
   db2_decision_log_row_t p1, p2;
   assert(db2_decision_log_record("shared", "a", "a", "r", "op", 1, "", 0, &p1) == 0);
   assert(db2_decision_log_record("shared", "a", "a", "r", "op", 2, "", 0, &p2) == 0);
   printf("  PASS: test_distinct_scopes_coexist\n");
}

/* Superseding a decision in a DIFFERENT scope (or a stale/nonexistent id) is
 * rejected — the record fails and the unrelated decision is untouched. */
static void test_supersede_wrong_scope_rejected(void)
{
   db2_decision_log_row_t a, a_after;
   assert(db2_decision_log_record("alpha", "a", "a", "r", "op", 0, "", 0, &a) == 0);
   /* try to supersede alpha's decision from a record in scope 'beta' */
   int rc = db2_decision_log_record("beta", "b", "b", "r", "op", 0, "", a.id, NULL);
   assert(rc != 0); /* wrong scope: UPDATE matches 0 rows -> rejected */
   assert(db2_decision_log_get(a.id, &a_after) == 0);
   assert(strcmp(a_after.status, "active") == 0); /* alpha untouched */
   /* a stale/nonexistent supersedes_id is likewise rejected */
   assert(db2_decision_log_record("gamma", "g", "g", "r", "op", 0, "", 999999, NULL) != 0);
   printf("  PASS: test_supersede_wrong_scope_rejected\n");
}

/* An empty subject is rejected (would otherwise be a single global active slot). */
static void test_empty_subject_rejected(void)
{
   assert(db2_decision_log_record("", "a", "a", "r", "op", 0, "", 0, NULL) != 0);
   printf("  PASS: test_empty_subject_rejected\n");
}

int main(void)
{
   db2_test_shim_open();
   test_record_is_active();
   test_supersede_flips_prior();
   test_one_active_per_scope();
   test_distinct_scopes_coexist();
   test_supersede_wrong_scope_rejected();
   test_empty_subject_rejected();
   db2_test_shim_close();
   printf("decision_log: all tests passed\n");
   return 0;
}
