/* test_delegate_plan.c: read-only delegate work-packet planner tests */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "delegate_plan.h"

static cJSON *arr(cJSON *obj, const char *name)
{
   cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, name);
   assert(cJSON_IsArray(v));
   return v;
}

static int num(cJSON *obj, const char *name)
{
   cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, name);
   assert(cJSON_IsNumber(v));
   return v->valueint;
}

static int str_arr_contains(cJSON *array, const char *expected)
{
   cJSON *item = NULL;
   cJSON_ArrayForEach(item, array)
   {
      if (cJSON_IsString(item) && strcmp(item->valuestring, expected) == 0)
         return 1;
   }
   return 0;
}

static int bool_value(cJSON *obj, const char *name)
{
   cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, name);
   assert(cJSON_IsBool(v));
   return cJSON_IsTrue(v);
}

static void test_plan_from_changes_table(void)
{
   const char *proposal =
       "# Proposal: Delegate Work Packet Planner\n"
       "\n"
       "### Changes\n"
       "\n"
       "| File | Change |\n"
       "|------|--------|\n"
       "| `src/cmd_agent_delegate.c` | Add plan subcommand. |\n"
       "| `src/modules/delegates/delegate_plan.c` | Add planner helpers. |\n"
       "\n"
       "## Acceptance Criteria\n"
       "\n"
       "- [ ] `aimee delegate plan <proposal.md>` emits valid JSON work packets.\n"
       "- [ ] Packet generation flags overlapping owned files before launch.\n";

   char err[256] = "";
   cJSON *plan = delegate_plan_build_from_text("docs/proposals/pending/delegate.md", proposal, err,
                                               sizeof(err));
   assert(plan != NULL);
   assert(strcmp(cJSON_GetObjectItem(plan, "schema")->valuestring, "delegate_plan_v1") == 0);
   assert(num(plan, "implementation_packet_count") == 2);
   assert(num(plan, "packet_count") == 3);
   assert(num(plan, "parallel_safe_count") == 2);
   assert(num(plan, "needs_sequencing_count") == 0);
   assert(cJSON_IsTrue(cJSON_GetObjectItem(plan, "reviewer_packet_included")));
   assert(cJSON_GetArraySize(arr(plan, "conflicts")) == 0);

   cJSON *packets = arr(plan, "packets");
   cJSON *first = cJSON_GetArrayItem(packets, 0);
   assert(strcmp(cJSON_GetObjectItem(first, "role")->valuestring, "code") == 0);
   assert(cJSON_GetArraySize(arr(first, "owned_files")) == 1);
   assert(cJSON_GetArraySize(arr(first, "acceptance_criteria")) == 2);
   /* The first packet owns src/cmd_agent_delegate.c, which has no matching unit
    * test, so it verifies with lint only — and must NOT carry an unrelated
    * test target. */
   assert(str_arr_contains(arr(first, "verify_commands"), "make -C src lint"));
   assert(!str_arr_contains(arr(first, "verify_commands"),
                            "make -C src build/obj/tests/unit-test-delegate-plan"));
   assert(!str_arr_contains(arr(first, "read_context"), "src/tests/test_delegate_plan.c"));
   /* The second packet owns src/modules/delegates/delegate_plan.c, whose test exists, so it
    * verifies against and reads exactly that test. */
   cJSON *second = cJSON_GetArrayItem(packets, 1);
   assert(str_arr_contains(arr(second, "verify_commands"),
                           "make -C src build/obj/tests/unit-test-delegate-plan"));
   assert(str_arr_contains(arr(second, "verify_commands"),
                           "./src/build/obj/tests/unit-test-delegate-plan"));
   assert(str_arr_contains(arr(second, "read_context"), "src/tests/test_delegate_plan.c"));
   assert(str_arr_contains(arr(first, "non_goals"),
                           "do not edit files outside owned_files without listing the ownership "
                           "drift"));
   assert(strcmp(cJSON_GetObjectItem(first, "handoff_schema")->valuestring, "delegate_result_v1") ==
          0);
   cJSON *contract = cJSON_GetObjectItem(first, "final_output_contract");
   assert(cJSON_IsString(contract));
   assert(strstr(contract->valuestring, "delegate_result_v1") != NULL);
   assert(strstr(contract->valuestring, "status=done") != NULL);

   cJSON *review = cJSON_GetArrayItem(packets, 2);
   assert(strcmp(cJSON_GetObjectItem(review, "role")->valuestring, "review") == 0);
   assert(cJSON_GetArraySize(arr(review, "owned_files")) == 0);
   assert(str_arr_contains(arr(review, "expected_files"), "src/cmd_agent_delegate.c"));
   assert(str_arr_contains(arr(review, "expected_files"), "src/modules/delegates/delegate_plan.c"));
   assert(strcmp(cJSON_GetObjectItem(review, "handoff_schema")->valuestring,
                 "delegate_review_v1") == 0);
   contract = cJSON_GetObjectItem(review, "final_output_contract");
   assert(cJSON_IsString(contract));
   assert(strstr(contract->valuestring, "delegate_review_v1") != NULL);
   assert(strstr(contract->valuestring, "owner_packet") != NULL);
   assert(strstr(contract->valuestring, "verification") != NULL);
   assert(strstr(contract->valuestring, "inspected current-code evidence") != NULL);

   cJSON_Delete(plan);
   printf("  PASS: test_plan_from_changes_table\n");
}

static void test_plan_flags_overlapping_owned_files(void)
{
   const char *proposal = "# Proposal: Conflict Example\n"
                          "\n"
                          "### Changes\n"
                          "| File | Change |\n"
                          "|------|--------|\n"
                          "| `src/modules/delegates/delegate_plan.c` | First slice. |\n"
                          "| `src/modules/delegates/delegate_plan.c` | Second slice. |\n"
                          "\n"
                          "## Acceptance Criteria\n"
                          "- [ ] Works.\n";

   char err[256] = "";
   cJSON *plan = delegate_plan_build_from_text("docs/p.md", proposal, err, sizeof(err));
   assert(plan != NULL);
   assert(num(plan, "implementation_packet_count") == 2);
   assert(num(plan, "needs_sequencing_count") == 1);
   assert(cJSON_GetArraySize(arr(plan, "conflicts")) == 1);
   cJSON *conflict = cJSON_GetArrayItem(arr(plan, "conflicts"), 0);
   assert(strcmp(cJSON_GetObjectItem(conflict, "file")->valuestring,
                 "src/modules/delegates/delegate_plan.c") == 0);
   assert(num(conflict, "packet_count") == 2);
   cJSON_Delete(plan);
   printf("  PASS: test_plan_flags_overlapping_owned_files\n");
}

static void test_changes_table_splits_multi_path_cell(void)
{
   const char *proposal =
       "# Proposal: Multi Path Cell\n"
       "\n"
       "### Changes\n"
       "| File | Change |\n"
       "|------|--------|\n"
       "| `src/headers/compute_pool.h`, `src/server/compute_pool.c` | Add worker slots. |\n"
       "| `src/server/server_state.c` | Render slots. |\n"
       "\n"
       "## Acceptance Criteria\n"
       "- [ ] All listed files receive work packets.\n";

   char err[256] = "";
   cJSON *plan = delegate_plan_build_from_text("docs/p.md", proposal, err, sizeof(err));
   assert(plan != NULL);
   assert(num(plan, "implementation_packet_count") == 3);

   cJSON *packets = arr(plan, "packets");
   assert(str_arr_contains(arr(cJSON_GetArrayItem(packets, 0), "owned_files"),
                           "src/headers/compute_pool.h"));
   assert(str_arr_contains(arr(cJSON_GetArrayItem(packets, 1), "owned_files"),
                           "src/server/compute_pool.c"));
   assert(str_arr_contains(arr(cJSON_GetArrayItem(packets, 2), "owned_files"),
                           "src/server/server_state.c"));
   cJSON_Delete(plan);
   printf("  PASS: test_changes_table_splits_multi_path_cell\n");
}

static void test_changes_table_canonicalizes_schema_paths(void)
{
   const char *proposal =
       "# Proposal: Cron DB Schema\n"
       "\n"
       "### Changes\n"
       "| File | Change |\n"
       "|------|--------|\n"
       "| `src/db1/db1_schema.sql` | Add cron job tables. |\n"
       "| `src/db2/db2_schema.sql`, `src/db2/db2_schema_sqlite.sql` | Mirror tables. |\n"
       "\n"
       "## Acceptance Criteria\n"
       "- [ ] Delegates receive existing schema files.\n";

   char err[256] = "";
   cJSON *plan = delegate_plan_build_from_text("docs/p.md", proposal, err, sizeof(err));
   assert(plan != NULL);
   assert(num(plan, "implementation_packet_count") == 3);

   cJSON *packets = arr(plan, "packets");
   assert(
       str_arr_contains(arr(cJSON_GetArrayItem(packets, 0), "owned_files"), "src/db1/schema.sql"));
   assert(
       str_arr_contains(arr(cJSON_GetArrayItem(packets, 1), "owned_files"), "src/db2/schema.sql"));
   assert(str_arr_contains(arr(cJSON_GetArrayItem(packets, 2), "owned_files"),
                           "src/db2/schema_sqlite.sql"));
   assert(
       str_arr_contains(arr(cJSON_GetArrayItem(packets, 0), "read_context"), "src/db1/schema.sql"));
   cJSON_Delete(plan);
   printf("  PASS: test_changes_table_canonicalizes_schema_paths\n");
}

static void test_schema_path_canonicalization_ignores_near_misses(void)
{
   const char *proposal = "# Proposal: Near Miss Paths\n"
                          "\n"
                          "### Changes\n"
                          "| File | Change |\n"
                          "|------|--------|\n"
                          "| `my_src/db1/db1_schema.sql` (new) | Leave unrelated path alone. |\n"
                          "| `src/db1/schema.sql` | Already canonical. |\n"
                          "\n"
                          "## Acceptance Criteria\n"
                          "- [ ] Only exact legacy schema paths are rewritten.\n";

   char err[256] = "";
   cJSON *plan = delegate_plan_build_from_text("docs/p.md", proposal, err, sizeof(err));
   assert(plan != NULL);
   assert(num(plan, "implementation_packet_count") == 2);

   cJSON *packets = arr(plan, "packets");
   assert(str_arr_contains(arr(cJSON_GetArrayItem(packets, 0), "owned_files"),
                           "my_src/db1/db1_schema.sql"));
   assert(
       str_arr_contains(arr(cJSON_GetArrayItem(packets, 1), "owned_files"), "src/db1/schema.sql"));
   cJSON_Delete(plan);
   printf("  PASS: test_schema_path_canonicalization_ignores_near_misses\n");
}

static void test_acceptance_criteria_preserve_wrapped_lines(void)
{
   const char *proposal = "# Proposal: Wrapped Acceptance\n"
                          "\n"
                          "### Changes\n"
                          "| File | Change |\n"
                          "|------|--------|\n"
                          "| `src/modules/delegates/delegate_plan.c` | Touch planner. |\n"
                          "\n"
                          "## Acceptance Criteria\n"
                          "- [ ] `aimee delegate code --via claude-cli \"Add a README\"` runs\n"
                          "      with write detection enabled.\n"
                          "- [ ] One reminder transitions from `armed` in session A to\n"
                          "      `completed` in session B.\n";

   char err[256] = "";
   cJSON *plan = delegate_plan_build_from_text("docs/p.md", proposal, err, sizeof(err));
   assert(plan != NULL);
   cJSON *packet = cJSON_GetArrayItem(arr(plan, "packets"), 0);
   cJSON *criteria = arr(packet, "acceptance_criteria");
   assert(cJSON_GetArraySize(criteria) == 2);
   assert(strstr(cJSON_GetArrayItem(criteria, 0)->valuestring,
                 "runs with write detection enabled") != NULL);
   assert(strstr(cJSON_GetArrayItem(criteria, 1)->valuestring, "to `completed` in session B") !=
          NULL);
   cJSON_Delete(plan);
   printf("  PASS: test_acceptance_criteria_preserve_wrapped_lines\n");
}

static void test_plan_falls_back_to_backticked_paths(void)
{
   const char *proposal = "# Proposal: Backtick Example\n"
                          "\n"
                          "Update `src/modules/delegates/delegate_plan.c` and document it in "
                          "`src/tests/test_delegate_plan.c`.\n"
                          "\n"
                          "## Acceptance Criteria\n"
                          "- [ ] Emits packets.\n";

   char err[256] = "";
   cJSON *plan = delegate_plan_build_from_text("docs/p.md", proposal, err, sizeof(err));
   assert(plan != NULL);
   assert(num(plan, "implementation_packet_count") == 2);
   assert(cJSON_GetArraySize(arr(plan, "packets")) == 3);
   cJSON_Delete(plan);
   printf("  PASS: test_plan_falls_back_to_backticked_paths\n");
}

static void test_changes_table_ignores_route_mentions(void)
{
   const char *proposal = "# Proposal: Trigger Example\n"
                          "\n"
                          "Expose `POST /v1/trigger` and document `/v1/trigger` behaviour. Mention "
                          "`scripts/relay.py` before the table.\n"
                          "\n"
                          "### Changes\n"
                          "| File | Change |\n"
                          "|------|--------|\n"
                          "| `src/server/server_trigger.c` | Handle `/v1/trigger`. |\n"
                          "| `scripts/relay.py` (new) | Relay webhooks to `POST /v1/trigger`. |\n"
                          "\n"
                          "## Acceptance Criteria\n"
                          "- [ ] `POST /v1/trigger` queues work.\n"
                          "- [ ] `/v1/trigger` without auth is rejected.\n";

   char err[256] = "";
   cJSON *plan = delegate_plan_build_from_text("docs/p.md", proposal, err, sizeof(err));
   assert(plan != NULL);
   assert(num(plan, "implementation_packet_count") == 2);
   assert(num(plan, "needs_sequencing_count") == 0);
   cJSON *packets = arr(plan, "packets");
   cJSON *first = cJSON_GetArrayItem(packets, 0);
   cJSON *second = cJSON_GetArrayItem(packets, 1);
   assert(str_arr_contains(arr(first, "owned_files"), "src/server/server_trigger.c"));
   assert(str_arr_contains(arr(second, "owned_files"), "scripts/relay.py"));
   cJSON_Delete(plan);
   printf("  PASS: test_changes_table_ignores_route_mentions\n");
}

static void test_backtick_fallback_rejects_route_literals(void)
{
   const char *proposal = "# Proposal: Routes Only\n"
                          "\n"
                          "Wire `src/server/server_trigger.c` for `POST /v1/trigger` and "
                          "`/v1/trigger`.\n";

   char err[256] = "";
   cJSON *plan = delegate_plan_build_from_text("docs/p.md", proposal, err, sizeof(err));
   assert(plan != NULL);
   assert(num(plan, "implementation_packet_count") == 1);
   cJSON *packet = cJSON_GetArrayItem(arr(plan, "packets"), 0);
   assert(str_arr_contains(arr(packet, "owned_files"), "src/server/server_trigger.c"));
   cJSON_Delete(plan);
   printf("  PASS: test_backtick_fallback_rejects_route_literals\n");
}

static void test_plan_ignores_template_artifact_paths(void)
{
   const char *proposal =
       "# Proposal: Operational Artefacts\n"
       "\n"
       "### Changes\n"
       "| File | Change |\n"
       "|------|--------|\n"
       "| `benchmarks/dogfood/<YYYY-MM>/report.json` | Monthly report artefact. |\n"
       "| `benchmarks/dogfood/<YYYY-MM>/reminder-demo.*.json` | Reminder evidence. |\n"
       "| `docs/proposals/pending/<new>.md` | Follow-up proposal. |\n"
       "\n"
       "## Acceptance Criteria\n"
       "- [ ] Operator commits concrete month-end artefacts.\n"
       "- [ ] One derived PR files a new proposal under `docs/proposals/pending/`.\n";

   char err[256] = "";
   cJSON *plan = delegate_plan_build_from_text("docs/p.md", proposal, err, sizeof(err));
   assert(plan != NULL);
   assert(num(plan, "implementation_packet_count") == 0);
   assert(num(plan, "parallel_safe_count") == 0);
   assert(num(plan, "needs_sequencing_count") == 0);
   assert(num(plan, "packet_count") == 1);
   assert(cJSON_IsTrue(cJSON_GetObjectItem(plan, "manual_only")));
   cJSON *reason = cJSON_GetObjectItem(plan, "no_implementation_reason");
   assert(cJSON_IsString(reason));
   assert(strstr(reason->valuestring, "No concrete owned files") != NULL);

   cJSON *packet = cJSON_GetArrayItem(arr(plan, "packets"), 0);
   assert(strcmp(cJSON_GetObjectItem(packet, "role")->valuestring, "review") == 0);
   assert(cJSON_GetArraySize(arr(packet, "owned_files")) == 0);
   assert(cJSON_GetArraySize(arr(packet, "expected_files")) == 0);
   assert(str_arr_contains(arr(packet, "read_context"),
                           "benchmarks/dogfood/<YYYY-MM>/report.json") == 0);
   cJSON_Delete(plan);
   printf("  PASS: test_plan_ignores_template_artifact_paths\n");
}

static void test_plan_keeps_new_files_out_of_read_context(void)
{
   const char *proposal =
       "# Proposal: New Cron File\n"
       "\n"
       "### Changes\n"
       "| File | Change |\n"
       "|------|--------|\n"
       "| `src/not-yet-created-delegate-plan-fixture.c` (new) | Add the implementation. |\n"
       "| `src/modules/delegates/delegate_plan.c` | Wire planner behavior. |\n"
       "\n"
       "## Acceptance Criteria\n"
       "- [ ] Delegates are not asked to preload missing owned files.\n";

   char err[256] = "";
   cJSON *plan = delegate_plan_build_from_text("docs/p.md", proposal, err, sizeof(err));
   assert(plan != NULL);
   assert(num(plan, "implementation_packet_count") == 2);

   cJSON *packets = arr(plan, "packets");
   cJSON *new_file_packet = cJSON_GetArrayItem(packets, 0);
   assert(str_arr_contains(arr(new_file_packet, "owned_files"),
                           "src/not-yet-created-delegate-plan-fixture.c"));
   assert(str_arr_contains(arr(new_file_packet, "read_context"),
                           "src/not-yet-created-delegate-plan-fixture.c") == 0);

   cJSON *existing_file_packet = cJSON_GetArrayItem(packets, 1);
   assert(str_arr_contains(arr(existing_file_packet, "read_context"),
                           "src/modules/delegates/delegate_plan.c"));

   cJSON *review = cJSON_GetArrayItem(packets, 2);
   assert(str_arr_contains(arr(review, "expected_files"),
                           "src/not-yet-created-delegate-plan-fixture.c"));
   assert(str_arr_contains(arr(review, "expected_files"), "src/modules/delegates/delegate_plan.c"));
   assert(str_arr_contains(arr(review, "read_context"),
                           "src/not-yet-created-delegate-plan-fixture.c") == 0);
   assert(str_arr_contains(arr(review, "read_context"), "src/modules/delegates/delegate_plan.c"));
   cJSON_Delete(plan);
   printf("  PASS: test_plan_keeps_new_files_out_of_read_context\n");
}

static void test_plan_flags_unmarked_missing_owned_files(void)
{
   const char *proposal = "# Proposal: Missing Owned File\n"
                          "\n"
                          "### Changes\n"
                          "| File | Change |\n"
                          "|------|--------|\n"
                          "| `src/modules/delegates/delegate_plan.c` | Existing planner work. |\n"
                          "| `src/does-not-exist-delegate-plan-fixture.c` | Suspect stale path. |\n"
                          "\n"
                          "## Acceptance Criteria\n"
                          "- [ ] Missing files require supervisor review before launch.\n";

   char err[256] = "";
   cJSON *plan = delegate_plan_build_from_text("docs/p.md", proposal, err, sizeof(err));
   assert(plan != NULL);
   assert(bool_value(plan, "requires_supervisor_review"));
   assert(bool_value(plan, "manual_only") == 0);
   assert(num(plan, "implementation_packet_count") == 1);
   assert(num(plan, "packet_count") == 2);
   assert(str_arr_contains(arr(plan, "missing_owned_files"),
                           "src/does-not-exist-delegate-plan-fixture.c"));
   assert(str_arr_contains(arr(plan, "new_owned_files"),
                           "src/does-not-exist-delegate-plan-fixture.c") == 0);
   cJSON *packets = arr(plan, "packets");
   cJSON *impl = cJSON_GetArrayItem(packets, 0);
   assert(str_arr_contains(arr(impl, "owned_files"), "src/modules/delegates/delegate_plan.c"));
   assert(str_arr_contains(arr(impl, "owned_files"),
                           "src/does-not-exist-delegate-plan-fixture.c") == 0);
   cJSON_Delete(plan);
   printf("  PASS: test_plan_flags_unmarked_missing_owned_files\n");
}

static void test_plan_makes_all_missing_unmarked_paths_manual_only(void)
{
   const char *proposal = "# Proposal: Stale File Names\n"
                          "\n"
                          "### Changes\n"
                          "| File | Change |\n"
                          "|------|--------|\n"
                          "| `src/old-cron-jobs.c` | Former scheduler file. |\n"
                          "| `src/db1/old-cron-jobs.c` | Former DB file. |\n"
                          "\n"
                          "## Acceptance Criteria\n"
                          "- [ ] Missing paths are reviewed before delegates launch.\n";

   char err[256] = "";
   cJSON *plan = delegate_plan_build_from_text("docs/p.md", proposal, err, sizeof(err));
   assert(plan != NULL);
   assert(bool_value(plan, "requires_supervisor_review"));
   assert(bool_value(plan, "manual_only"));
   assert(num(plan, "implementation_packet_count") == 0);
   assert(num(plan, "packet_count") == 1);
   assert(cJSON_GetArraySize(arr(plan, "missing_owned_files")) == 2);
   cJSON *review = cJSON_GetArrayItem(arr(plan, "packets"), 0);
   assert(strcmp(cJSON_GetObjectItem(review, "role")->valuestring, "review") == 0);
   assert(str_arr_contains(arr(review, "expected_files"), "src/old-cron-jobs.c"));
   assert(str_arr_contains(arr(review, "expected_files"), "src/db1/old-cron-jobs.c"));
   cJSON_Delete(plan);
   printf("  PASS: test_plan_makes_all_missing_unmarked_paths_manual_only\n");
}

static void test_plan_allows_declared_new_owned_files(void)
{
   const char *proposal =
       "# Proposal: New Owned File\n"
       "\n"
       "### Changes\n"
       "| File | Change |\n"
       "|------|--------|\n"
       "| `src/brand-new-delegate-plan-fixture.c` (new) | Add implementation. |\n"
       "| `src/modules/delegates/delegate_plan.c` | Wire planner behavior. |\n"
       "\n"
       "## Acceptance Criteria\n"
       "- [ ] Declared new files can launch without missing-file review.\n";

   char err[256] = "";
   cJSON *plan = delegate_plan_build_from_text("docs/p.md", proposal, err, sizeof(err));
   assert(plan != NULL);
   assert(bool_value(plan, "requires_supervisor_review") == 0);
   assert(str_arr_contains(arr(plan, "new_owned_files"), "src/brand-new-delegate-plan-fixture.c"));
   assert(str_arr_contains(arr(plan, "missing_owned_files"),
                           "src/brand-new-delegate-plan-fixture.c") == 0);
   cJSON *packets = arr(plan, "packets");
   cJSON *new_file_packet = cJSON_GetArrayItem(packets, 0);
   assert(str_arr_contains(arr(new_file_packet, "owned_files"),
                           "src/brand-new-delegate-plan-fixture.c"));
   assert(str_arr_contains(arr(new_file_packet, "read_context"),
                           "src/brand-new-delegate-plan-fixture.c") == 0);
   cJSON_Delete(plan);
   printf("  PASS: test_plan_allows_declared_new_owned_files\n");
}

static void test_large_plan_gets_reviewer_packet(void)
{
   char proposal[2600];
   int len =
       snprintf(proposal, sizeof(proposal),
                "# Proposal: Large Example\n\nUpdate `src/modules/delegates/delegate_plan.c`.\n\n");
   assert(len > 0 && (size_t)len < sizeof(proposal));
   memset(proposal + len, 'a', sizeof(proposal) - (size_t)len - 1);
   proposal[sizeof(proposal) - 1] = '\0';

   char err[256] = "";
   cJSON *plan = delegate_plan_build_from_text("docs/p.md", proposal, err, sizeof(err));
   assert(plan != NULL);
   assert(num(plan, "implementation_packet_count") == 1);
   assert(num(plan, "packet_count") == 2);
   assert(cJSON_IsTrue(cJSON_GetObjectItem(plan, "reviewer_packet_included")));
   cJSON_Delete(plan);
   printf("  PASS: test_large_plan_gets_reviewer_packet\n");
}

int main(void)
{
   printf("test_delegate_plan\n");
   test_plan_from_changes_table();
   test_plan_flags_overlapping_owned_files();
   test_changes_table_splits_multi_path_cell();
   test_changes_table_canonicalizes_schema_paths();
   test_schema_path_canonicalization_ignores_near_misses();
   test_acceptance_criteria_preserve_wrapped_lines();
   test_plan_falls_back_to_backticked_paths();
   test_changes_table_ignores_route_mentions();
   test_backtick_fallback_rejects_route_literals();
   test_plan_ignores_template_artifact_paths();
   test_plan_keeps_new_files_out_of_read_context();
   test_plan_flags_unmarked_missing_owned_files();
   test_plan_makes_all_missing_unmarked_paths_manual_only();
   test_plan_allows_declared_new_owned_files();
   test_large_plan_gets_reviewer_packet();
   printf("All delegate plan tests passed.\n");
   return 0;
}
