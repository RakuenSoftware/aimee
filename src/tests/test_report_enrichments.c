/* test_report_enrichments.c: DB2 report enrichment cache tests. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "report_enrichment.h"
#include "report_enrichments.h"
#include "modules/db2/c/db2_test_shim.h"
#include "support/json_canonical.h"

static void open_db(void)
{
   db2_test_shim_close();
   db2_test_shim_open();
}

static void close_db(void)
{
   db2_test_shim_close();
}

static void test_upsert_read_and_conflict_update(void)
{
   open_db();

   report_subject_t subject;
   assert(report_subject_from_git_remote("git@github.com:Org/Repo.git", &subject) == 0);
   assert(db2_report_enrichment_upsert(&subject, "repository_profile", "unit", "v1",
                                       "{\"score\":1}", "h1", "2026-05-16 12:00:00",
                                       "2026-05-17 00:00:00") == 0);

   db2_report_enrichment_row_t row;
   assert(db2_report_enrichment_read(&subject, "repository_profile", "unit", "v1", &row) == 0);
   assert(strcmp(row.subject.type, REPORT_SUBJECT_TYPE_GIT_REPO) == 0);
   assert(strcmp(row.subject.id, "https://github.com/org/repo") == 0);
   assert(strstr(json_canonical(row.payload_json), "\"score\":1") != NULL);
   assert(strcmp(row.input_hash, "h1") == 0);
   assert(db2_report_enrichment_is_expired(&row, "2026-05-16 23:59:59") == 0);
   assert(db2_report_enrichment_is_expired(&row, "2026-05-17 00:00:00") == 1);

   assert(db2_report_enrichment_upsert(&subject, "repository_profile", "unit", "v1",
                                       "{\"score\":2}", "h2", "2026-05-16 13:00:00", "") == 0);
   assert(db2_report_enrichment_read(&subject, "repository_profile", "unit", "v1", &row) == 0);
   assert(strstr(json_canonical(row.payload_json), "\"score\":2") != NULL);
   assert(strcmp(row.input_hash, "h2") == 0);
   assert(strcmp(row.expires_at, "") == 0);
   assert(db2_report_enrichment_is_expired(&row, "2026-05-18 00:00:00") == 0);

   close_db();
   printf("  PASS: upsert_read_and_conflict_update\n");
}

static void test_read_miss_and_invalid_inputs(void)
{
   open_db();

   report_subject_t subject;
   assert(report_subject_from_git_org_url("https://github.com/Org", &subject) == 0);
   db2_report_enrichment_row_t row;
   assert(db2_report_enrichment_read(&subject, "organization_profile", "unit", "v1", &row) == -1);
   assert(db2_report_enrichment_upsert(&subject, "", "unit", "v1", "{}", "", NULL, NULL) == -1);
   assert(db2_report_enrichment_upsert(&subject, "organization_profile", "unit", "", "{}", "", NULL,
                                       NULL) == -1);

   report_subject_t aggregate = {0};
   snprintf(aggregate.type, sizeof(aggregate.type), "%s", REPORT_SUBJECT_TYPE_AGGREGATE);
   snprintf(aggregate.id, sizeof(aggregate.id), "%s", "workspace:test");
   assert(db2_report_enrichment_upsert(&aggregate, "workspace_profile", "unit", "v1", "{}", "",
                                       NULL, NULL) == -1);

   close_db();
   printf("  PASS: read_miss_and_invalid_inputs\n");
}

int main(void)
{
   printf("Running report_enrichments tests\n");
   test_upsert_read_and_conflict_update();
   test_read_miss_and_invalid_inputs();
   printf("All report_enrichments tests passed.\n");
   return 0;
}
