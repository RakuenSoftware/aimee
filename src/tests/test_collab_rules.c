/* test_collab_rules.c: unit tests for the collaborative agent rules subsystem */
#include <assert.h>
#include "db1.h"
#include "modules/db2/c/db2.h"
#include "modules/db2/c/db2_test_shim.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "aimee.h"
#include "collab_rules.h"

/* --- Helpers --- */

static void setup(void)
{
   db2_test_shim_close();
   assert(db1_init(":memory:") == 0);
   db2_test_shim_open();
}

static void teardown(void)
{
   db2_test_shim_close();
   db1_shutdown();
}

/* --- Tests: propose --- */

static void test_propose_returns_id(void)
{
   setup();

   int id = db2_collab_rules_propose("Always run tests before marking a task complete",
                                     "Prevents shipping broken code", "agent-a");
   assert(id > 0);

   teardown();
}

static void test_propose_empty_text_rejected(void)
{
   setup();

   int id = db2_collab_rules_propose("", "some reason", "agent");
   assert(id == -1);

   id = db2_collab_rules_propose(NULL, "some reason", "agent");
   assert(id == -1);

   teardown();
}

static void test_propose_text_too_long_rejected(void)
{
   setup();

   /* Build a string of COLLAB_RULE_TEXT_LEN + 1 characters */
   char long_text[COLLAB_RULE_TEXT_LEN + 2];
   memset(long_text, 'x', COLLAB_RULE_TEXT_LEN + 1);
   long_text[COLLAB_RULE_TEXT_LEN + 1] = '\0';

   int id = db2_collab_rules_propose(long_text, "reason", "agent");
   assert(id == -1);

   teardown();
}

static void test_propose_reason_too_long_rejected(void)
{
   setup();

   char long_reason[COLLAB_RULE_REASON_LEN + 2];
   memset(long_reason, 'y', COLLAB_RULE_REASON_LEN + 1);
   long_reason[COLLAB_RULE_REASON_LEN + 1] = '\0';

   int id = db2_collab_rules_propose("Valid text", long_reason, "agent");
   assert(id == -1);

   teardown();
}

static void test_propose_at_text_limit_accepted(void)
{
   setup();

   /* Exactly COLLAB_RULE_TEXT_LEN characters — should succeed */
   char exact_text[COLLAB_RULE_TEXT_LEN + 1];
   memset(exact_text, 'z', COLLAB_RULE_TEXT_LEN);
   exact_text[COLLAB_RULE_TEXT_LEN] = '\0';

   int id = db2_collab_rules_propose(exact_text, "reason", "agent");
   assert(id > 0);

   teardown();
}

static void test_propose_proposed_status(void)
{
   setup();

   int id = db2_collab_rules_propose("Use feature branches only", "Safety", "agent");
   assert(id > 0);

   collab_rule_t rules[COLLAB_MAX_TOTAL_RULES];
   int n = db2_collab_rules_list(rules, COLLAB_MAX_TOTAL_RULES);
   assert(n == 1);
   assert(rules[0].status == COLLAB_PROPOSED);
   assert(strcmp(rules[0].proposed_by, "agent") == 0);
   assert(strcmp(rules[0].text, "Use feature branches only") == 0);

   teardown();
}

/* --- Tests: approve --- */

static void test_approve_makes_rule_active(void)
{
   setup();

   int id = db2_collab_rules_propose("Run lint before committing", "Quality", "agent");
   assert(id > 0);

   int rc = db2_collab_rules_approve(id);
   assert(rc == 0);

   collab_rule_t rules[COLLAB_MAX_TOTAL_RULES];
   int n = db2_collab_rules_list(rules, COLLAB_MAX_TOTAL_RULES);
   assert(n == 1);
   assert(rules[0].status == COLLAB_ACTIVE);
   assert(rules[0].decided_at[0] != '\0');

   teardown();
}

static void test_approve_nonexistent_fails(void)
{
   setup();

   int rc = db2_collab_rules_approve(9999);
   assert(rc == -1);

   teardown();
}

static void test_approve_increments_epoch(void)
{
   setup();

   int epoch_before = db2_collab_rules_epoch();

   int id = db2_collab_rules_propose("Keep PRs small", "Easier review", "agent");
   assert(id > 0);
   /* Proposing must NOT increment the epoch */
   assert(db2_collab_rules_epoch() == epoch_before);

   int rc = db2_collab_rules_approve(id);
   assert(rc == 0);
   /* Approving MUST increment the epoch */
   assert(db2_collab_rules_epoch() == epoch_before + 1);

   teardown();
}

/* --- Tests: reject --- */

static void test_reject_makes_rule_rejected(void)
{
   setup();

   int id = db2_collab_rules_propose("Never use tabs", "Consistency", "agent");
   assert(id > 0);

   int rc = db2_collab_rules_reject(id);
   assert(rc == 0);

   collab_rule_t rules[COLLAB_MAX_TOTAL_RULES];
   db2_collab_rules_list(rules, COLLAB_MAX_TOTAL_RULES);
   assert(rules[0].status == COLLAB_REJECTED);

   teardown();
}

static void test_reject_does_not_increment_epoch(void)
{
   setup();

   int epoch_before = db2_collab_rules_epoch();

   int id = db2_collab_rules_propose("No force push", "Safety", "agent");
   assert(id > 0);

   db2_collab_rules_reject(id);

   /* Epoch must remain unchanged after rejection */
   assert(db2_collab_rules_epoch() == epoch_before);

   teardown();
}

static void test_reject_nonexistent_fails(void)
{
   setup();

   int rc = db2_collab_rules_reject(9999);
   assert(rc == -1);

   teardown();
}

static void test_reject_active_rule_fails(void)
{
   setup();

   int id = db2_collab_rules_propose("Write docs", "Clarity", "agent");
   assert(id > 0);
   db2_collab_rules_approve(id);

   /* Cannot reject an already-active rule */
   int rc = db2_collab_rules_reject(id);
   assert(rc == -1);

   teardown();
}

/* --- Tests: retire --- */

static void test_retire_makes_rule_retired(void)
{
   setup();

   int id = db2_collab_rules_propose("Use snake_case", "Convention", "agent");
   assert(id > 0);
   db2_collab_rules_approve(id);

   int rc = db2_collab_rules_retire(id);
   assert(rc == 0);

   collab_rule_t rules[COLLAB_MAX_TOTAL_RULES];
   db2_collab_rules_list(rules, COLLAB_MAX_TOTAL_RULES);
   assert(rules[0].status == COLLAB_RETIRED);

   teardown();
}

static void test_retire_increments_epoch(void)
{
   setup();

   int id = db2_collab_rules_propose("Log all errors", "Observability", "agent");
   assert(id > 0);
   db2_collab_rules_approve(id);
   int epoch_after_approve = db2_collab_rules_epoch();

   db2_collab_rules_retire(id);

   /* Retiring an active rule MUST increment the epoch */
   assert(db2_collab_rules_epoch() == epoch_after_approve + 1);

   teardown();
}

static void test_retire_proposed_rule_fails(void)
{
   setup();

   int id = db2_collab_rules_propose("Comment all functions", "Docs", "agent");
   assert(id > 0);

   /* Cannot retire a proposed rule — must approve first */
   int rc = db2_collab_rules_retire(id);
   assert(rc == -1);

   teardown();
}

/* --- Tests: limits --- */

static void test_max_active_rules_enforced(void)
{
   setup();

   /* Approve exactly COLLAB_MAX_ACTIVE_RULES rules */
   for (int i = 0; i < COLLAB_MAX_ACTIVE_RULES; i++)
   {
      char text[64];
      snprintf(text, sizeof(text), "Rule number %d — keep it clear", i);
      int id = db2_collab_rules_propose(text, "batch", "agent");
      assert(id > 0);
      int rc = db2_collab_rules_approve(id);
      assert(rc == 0);
   }

   /* The 11th approval must fail */
   int id = db2_collab_rules_propose("One too many rules for the system", "overflow", "agent");
   assert(id > 0); /* proposing is still allowed */
   int rc = db2_collab_rules_approve(id);
   assert(rc == -1); /* approval must be blocked */

   /* Active count must still be exactly the limit */
   collab_rule_t active[COLLAB_MAX_ACTIVE_RULES + 5];
   int n = db2_collab_rules_list_active(active, COLLAB_MAX_ACTIVE_RULES + 5);
   assert(n == COLLAB_MAX_ACTIVE_RULES);

   teardown();
}

static void test_max_total_rules_enforced(void)
{
   setup();

   /* Propose COLLAB_MAX_TOTAL_RULES rules (some proposed, some rejected) */
   for (int i = 0; i < COLLAB_MAX_TOTAL_RULES; i++)
   {
      char text[64];
      snprintf(text, sizeof(text), "Total rule %d padded to length ok", i);
      int id = db2_collab_rules_propose(text, "fill", "agent");
      assert(id > 0);
      if (i % 3 == 0)
         db2_collab_rules_reject(id);
   }

   /* Proposing beyond the total limit must fail */
   int id = db2_collab_rules_propose("Cannot add any more rules now", "overflow", "agent");
   assert(id == -1);

   teardown();
}

/* --- Tests: epoch --- */

static void test_initial_epoch_is_zero(void)
{
   setup();

   int epoch = db2_collab_rules_epoch();
   assert(epoch == 0);

   teardown();
}

static void test_epoch_only_increments_on_approve_and_retire(void)
{
   setup();

   /* propose: no increment */
   int id1 = db2_collab_rules_propose("Alpha rule here", "a", "agent");
   assert(db2_collab_rules_epoch() == 0);

   /* reject: no increment */
   int id2 = db2_collab_rules_propose("Beta rule here ok", "b", "agent");
   db2_collab_rules_reject(id2);
   assert(db2_collab_rules_epoch() == 0);

   /* approve: +1 */
   db2_collab_rules_approve(id1);
   assert(db2_collab_rules_epoch() == 1);

   /* retire: +1 */
   db2_collab_rules_retire(id1);
   assert(db2_collab_rules_epoch() == 2);

   teardown();
}

/* --- Tests: injection --- */

static void test_inject_force_returns_rules(void)
{
   setup();

   int id = db2_collab_rules_propose("Document public APIs always", "clarity", "agent");
   db2_collab_rules_approve(id);

   /* agent_last_epoch = -1 forces injection regardless of sync state */
   char *injected = db2_collab_rules_inject(-1);
   assert(injected != NULL);
   assert(strstr(injected, "Document public APIs always") != NULL);
   assert(strstr(injected, "epoch") != NULL);
   free(injected);

   teardown();
}

static void test_inject_returns_null_when_synced(void)
{
   setup();

   int id = db2_collab_rules_propose("Always write unit tests", "quality", "agent");
   db2_collab_rules_approve(id);

   int current_epoch = db2_collab_rules_epoch();

   /* Agent already at current epoch — no re-injection needed */
   char *injected = db2_collab_rules_inject(current_epoch);
   assert(injected == NULL);

   teardown();
}

static void test_inject_returns_rules_after_epoch_change(void)
{
   setup();

   int id = db2_collab_rules_propose("Prefer small functions always", "quality", "agent");
   db2_collab_rules_approve(id);
   int epoch_v1 = db2_collab_rules_epoch();

   /* First trigger: agent gets the rules */
   char *first = db2_collab_rules_inject(-1);
   assert(first != NULL);
   free(first);

   /* Now retire the rule and approve a new one — epoch advances */
   db2_collab_rules_retire(id);
   int id2 = db2_collab_rules_propose("Avoid global variables always", "safety", "agent");
   db2_collab_rules_approve(id2);
   assert(db2_collab_rules_epoch() > epoch_v1);

   /* Agent was at epoch_v1 — must get re-injection now */
   char *second = db2_collab_rules_inject(epoch_v1);
   assert(second != NULL);
   assert(strstr(second, "Avoid global variables always") != NULL);
   free(second);

   teardown();
}

static void test_inject_null_when_no_active_rules(void)
{
   setup();

   /* No approved rules — inject returns NULL */
   char *injected = db2_collab_rules_inject(-1);
   assert(injected == NULL);

   teardown();
}

/* --- Tests: list functions --- */

static void test_list_active_excludes_non_active(void)
{
   setup();

   int id1 = db2_collab_rules_propose("Use const wherever possible here", "style", "agent");
   int id2 = db2_collab_rules_propose("Avoid magic numbers in code", "readability", "agent");
   int id3 = db2_collab_rules_propose("Keep imports sorted alphabetically", "style", "agent");

   db2_collab_rules_approve(id1);
   db2_collab_rules_reject(id2);
   /* id3 stays proposed */
   (void)id3;

   collab_rule_t active[COLLAB_MAX_ACTIVE_RULES];
   int n = db2_collab_rules_list_active(active, COLLAB_MAX_ACTIVE_RULES);
   assert(n == 1);
   assert(strcmp(active[0].text, "Use const wherever possible here") == 0);

   teardown();
}

static void test_list_all_includes_all_statuses(void)
{
   setup();

   int id1 = db2_collab_rules_propose("Descriptive variable names please", "clarity", "a");
   int id2 = db2_collab_rules_propose("No commented-out code in PRs ok", "hygiene", "b");
   int id3 = db2_collab_rules_propose("Add changelog entry for features", "process", "c");

   db2_collab_rules_approve(id1);
   db2_collab_rules_reject(id2);
   /* id3 stays proposed */
   (void)id3;

   collab_rule_t all[COLLAB_MAX_TOTAL_RULES];
   int n = db2_collab_rules_list(all, COLLAB_MAX_TOTAL_RULES);
   assert(n == 3);

   teardown();
}

/* --- Tests: JSON serialization --- */

static void test_json_active_includes_epoch(void)
{
   setup();

   int id = db2_collab_rules_propose("Review all PRs before merging", "quality", "bot");
   db2_collab_rules_approve(id);

   char *json = db2_collab_rules_json_active();
   assert(json != NULL);
   assert(strstr(json, "\"epoch\"") != NULL);
   assert(strstr(json, "\"rules\"") != NULL);
   assert(strstr(json, "Review all PRs before merging") != NULL);
   free(json);

   teardown();
}

static void test_json_all_includes_all_statuses(void)
{
   setup();

   int id1 = db2_collab_rules_propose("Write commit messages in imperative", "convention", "a");
   int id2 = db2_collab_rules_propose("Squash fixup commits before merge", "hygiene", "b");
   db2_collab_rules_approve(id1);
   db2_collab_rules_reject(id2);

   char *json = db2_collab_rules_json_all();
   assert(json != NULL);
   assert(strstr(json, "active") != NULL);
   assert(strstr(json, "rejected") != NULL);
   free(json);

   teardown();
}

static void test_json_active_empty_when_no_rules(void)
{
   setup();

   char *json = db2_collab_rules_json_active();
   assert(json != NULL);
   assert(strstr(json, "\"rules\":[]") != NULL);
   free(json);

   teardown();
}

int main(void)
{
   printf("collab_rules: ");

   test_propose_returns_id();
   test_propose_empty_text_rejected();
   test_propose_text_too_long_rejected();
   test_propose_reason_too_long_rejected();
   test_propose_at_text_limit_accepted();
   test_propose_proposed_status();

   test_approve_makes_rule_active();
   test_approve_nonexistent_fails();
   test_approve_increments_epoch();

   test_reject_makes_rule_rejected();
   test_reject_does_not_increment_epoch();
   test_reject_nonexistent_fails();
   test_reject_active_rule_fails();

   test_retire_makes_rule_retired();
   test_retire_increments_epoch();
   test_retire_proposed_rule_fails();

   test_max_active_rules_enforced();
   test_max_total_rules_enforced();

   test_initial_epoch_is_zero();
   test_epoch_only_increments_on_approve_and_retire();

   test_inject_force_returns_rules();
   test_inject_returns_null_when_synced();
   test_inject_returns_rules_after_epoch_change();
   test_inject_null_when_no_active_rules();

   test_list_active_excludes_non_active();
   test_list_all_includes_all_statuses();

   test_json_active_includes_epoch();
   test_json_all_includes_all_statuses();
   test_json_active_empty_when_no_rules();

   printf("all tests passed\n");
   return 0;
}
