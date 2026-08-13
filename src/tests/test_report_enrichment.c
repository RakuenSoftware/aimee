/* test_report_enrichment.c: unit tests for report enrichment subject identity. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <unistd.h>
#endif

#include "cJSON.h"
#include "report_enrichment.h"
#include "platform_test_util.h" /* platform_tmpdir: honour TMPDIR, do not leak into /tmp */

#define PASS(name) printf("  PASS: %s\n", name)

static void assert_subject(const report_subject_t *s, const char *type, const char *id,
                           const char *display)
{
   assert(s != NULL);
   assert(strcmp(s->type, type) == 0);
   assert(strcmp(s->id, id) == 0);
   assert(strcmp(s->display, display) == 0);
}

static void test_git_repo_subject_normalizes_remote(void)
{
   report_subject_t s;
   assert(report_subject_from_git_remote("git@github.com:Org/Repo.git", &s) == 0);
   assert_subject(&s, REPORT_SUBJECT_TYPE_GIT_REPO, "https://github.com/org/repo",
                  "github.com/org/repo");
   assert(report_subject_is_local(&s) == 0);

   assert(report_subject_from_git_remote("ssh://git@gitlab.com/Group/SubGroup/Repo.git", &s) == 0);
   assert_subject(&s, REPORT_SUBJECT_TYPE_GIT_REPO, "https://gitlab.com/group/subgroup/repo",
                  "gitlab.com/group/subgroup/repo");
   PASS("git_repo_subject_normalizes_remote");
}

static void test_git_org_subject_uses_explicit_scope(void)
{
   report_subject_t s;
   assert(report_subject_from_git_org_url("https://github.com/Org", &s) == 0);
   assert_subject(&s, REPORT_SUBJECT_TYPE_GIT_ORG, "https://github.com/org", "github.com/org");

   assert(report_subject_from_git_org_url("https://gitlab.com/Group/SubGroup", &s) == 0);
   assert_subject(&s, REPORT_SUBJECT_TYPE_GIT_ORG, "https://gitlab.com/group/subgroup",
                  "gitlab.com/group/subgroup");
   PASS("git_org_subject_uses_explicit_scope");
}

static void test_local_repo_subject(void)
{
   report_subject_t s;
   assert(report_subject_from_local_repo_id("abc123", &s) == 0);
   assert_subject(&s, REPORT_SUBJECT_TYPE_GIT_REPO, "local:abc123", "local:abc123");
   assert(report_subject_is_local(&s) == 1);

   assert(report_subject_from_local_repo_id("local:def456", &s) == 0);
   assert_subject(&s, REPORT_SUBJECT_TYPE_GIT_REPO, "local:def456", "local:def456");
   assert(report_subject_is_local(&s) == 1);
   PASS("local_repo_subject");
}

static void test_invalid_inputs(void)
{
   report_subject_t s;
   assert(report_subject_from_git_remote(NULL, &s) == -1);
   assert(report_subject_from_git_remote("", &s) == -1);
   assert(report_subject_from_git_remote("not-a-url", &s) == -1);
   assert(report_subject_from_git_remote("https://github.com", &s) == -1);

   assert(report_subject_from_git_org_url(NULL, &s) == -1);
   assert(report_subject_from_git_org_url("", &s) == -1);
   assert(report_subject_from_git_org_url("file:///tmp/org", &s) == -1);

   assert(report_subject_from_local_repo_id(NULL, &s) == -1);
   assert(report_subject_from_local_repo_id("", &s) == -1);
   PASS("invalid_inputs");
}

static void test_subject_json_helpers(void)
{
   report_subject_t repo;
   report_subject_t org;
   assert(report_subject_from_git_remote("git@github.com:Org/Repo.git", &repo) == 0);
   assert(report_subject_from_git_org_url("https://github.com/Org", &org) == 0);

   cJSON *obj = cJSON_CreateObject();
   assert(report_subject_add_json(obj, &repo) == 0);
   cJSON *subject = cJSON_GetObjectItemCaseSensitive(obj, "subject");
   assert(cJSON_IsObject(subject));
   assert(strcmp(cJSON_GetObjectItemCaseSensitive(subject, "type")->valuestring, "git_repo") == 0);
   assert(strcmp(cJSON_GetObjectItemCaseSensitive(subject, "id")->valuestring,
                 "https://github.com/org/repo") == 0);

   report_subject_t children[2] = {repo, org};
   assert(report_subject_add_aggregate_json(obj, "workspace:unit", children, 2) == 0);
   subject = cJSON_GetObjectItemCaseSensitive(obj, "subject");
   assert(strcmp(cJSON_GetObjectItemCaseSensitive(subject, "type")->valuestring, "aggregate") == 0);
   cJSON *arr = cJSON_GetObjectItemCaseSensitive(subject, "children");
   assert(cJSON_IsArray(arr));
   assert(cJSON_GetArraySize(arr) == 2);
   cJSON_Delete(obj);

   PASS("subject_json_helpers");
}

#ifndef _WIN32
static void cleanup_temp_dir(const char *dir)
{
   if (!dir || !dir[0])
      return;
   char cmd[1024];
   snprintf(cmd, sizeof(cmd), "rm -rf %s", dir);
   (void)system(cmd);
}

static void test_project_root_subject_reads_origin(void)
{
   char templ[256];
   snprintf(templ, sizeof templ, "%s/aimee-report-subject-XXXXXX", platform_tmpdir());
   char *dir = mkdtemp(templ);
   assert(dir != NULL);

   char cmd[1024];
   snprintf(cmd, sizeof(cmd), "git -C %s init -q", dir);
   assert(system(cmd) == 0);
   snprintf(cmd, sizeof(cmd), "git -C %s remote add origin git@github.com:Org/Repo.git", dir);
   assert(system(cmd) == 0);

   report_subject_t s;
   assert(report_subject_from_project_root(dir, &s) == 0);
   assert_subject(&s, REPORT_SUBJECT_TYPE_GIT_REPO, "https://github.com/org/repo",
                  "github.com/org/repo");

   cleanup_temp_dir(dir);
   PASS("project_root_subject_reads_origin");
}
#endif

int main(void)
{
   printf("Running report_enrichment tests\n");
   test_git_repo_subject_normalizes_remote();
   test_git_org_subject_uses_explicit_scope();
   test_local_repo_subject();
   test_invalid_inputs();
   test_subject_json_helpers();
#ifndef _WIN32
   test_project_root_subject_reads_origin();
#endif
   printf("All report_enrichment tests passed.\n");
   return 0;
}
