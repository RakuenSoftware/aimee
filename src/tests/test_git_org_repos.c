/* test_git_org_repos.c — provider detection + repo-list JSON parsing + the
 * paginated enumeration path (with a stubbed HTTP layer). No network. */

#include "git_org_repos.h"
#include "cJSON.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Stubs for the two externals git_org_repos.c references ─────────────────── */

/* No stored token in tests → enumerate unauthenticated. */
int git_host_cred_get(const char *host, char *out, size_t out_len)
{
   (void)host;
   if (out && out_len)
      out[0] = '\0';
   return 0;
}

/* Canned HTTP: page 1 of a GitHub org returns two repos; any other page/URL
 * returns an empty array so pagination stops. Records the last URL for assertions. */
static char g_last_url[1024];
static const char *g_page1_body = "[]";

int agent_http_get(const char *url, const char *extra_headers, char **response_buf, int timeout_ms)
{
   (void)extra_headers;
   (void)timeout_ms;
   snprintf(g_last_url, sizeof(g_last_url), "%s", url ? url : "");
   const char *body = (url && strstr(url, "page=1")) ? g_page1_body : "[]";
   *response_buf = strdup(body);
   return 200;
}

/* ── Tests ──────────────────────────────────────────────────────────────────── */

static int repo_count(cJSON *arr) { return cJSON_GetArraySize(arr); }

static cJSON *repo_at(cJSON *arr, int i) { return cJSON_GetArrayItem(arr, i); }

static const char *sval(cJSON *o, const char *k)
{
   cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
   return (cJSON_IsString(v) && v->valuestring) ? v->valuestring : "";
}

static void test_detect(void)
{
   assert(git_org_detect("github.com") == GIT_ORG_GITHUB);
   assert(git_org_detect("gitlab.com") == GIT_ORG_GITLAB);
   assert(git_org_detect("gitlab.example.com") == GIT_ORG_GITLAB);
   assert(git_org_detect("bitbucket.org") == GIT_ORG_BITBUCKET);
   assert(git_org_detect("gitea.example.com") == GIT_ORG_GITEA);
   assert(git_org_detect("git.mycompany.io") == GIT_ORG_GITEA); /* self-hosted default */
   assert(git_org_detect("") == GIT_ORG_UNKNOWN);
   assert(git_org_detect(NULL) == GIT_ORG_UNKNOWN);
   assert(strcmp(git_org_provider_name(GIT_ORG_GITHUB), "github") == 0);
   assert(strcmp(git_org_provider_name(GIT_ORG_BITBUCKET), "bitbucket") == 0);
   printf("  detect: OK\n");
}

static void test_parse_github(void)
{
   const char *body =
       "[{\"name\":\"repo-a\",\"clone_url\":\"https://github.com/o/repo-a.git\","
       "\"ssh_url\":\"git@github.com:o/repo-a.git\",\"private\":true},"
       "{\"name\":\"repo-b\",\"clone_url\":\"https://github.com/o/repo-b.git\","
       "\"ssh_url\":\"git@github.com:o/repo-b.git\",\"private\":false}]";
   cJSON *out = cJSON_CreateArray();
   int n = git_org_parse(GIT_ORG_GITHUB, body, out);
   assert(n == 2 && repo_count(out) == 2);
   assert(strcmp(sval(repo_at(out, 0), "name"), "repo-a") == 0);
   assert(strcmp(sval(repo_at(out, 0), "clone_url"), "https://github.com/o/repo-a.git") == 0);
   assert(cJSON_IsTrue(cJSON_GetObjectItem(repo_at(out, 0), "private")));
   assert(cJSON_IsFalse(cJSON_GetObjectItem(repo_at(out, 1), "private")));
   cJSON_Delete(out);
   printf("  parse github: OK\n");
}

static void test_parse_gitlab(void)
{
   const char *body =
       "[{\"name\":\"P A\",\"path\":\"proj-a\",\"http_url_to_repo\":\"https://gitlab.com/g/proj-a.git\","
       "\"ssh_url_to_repo\":\"git@gitlab.com:g/proj-a.git\",\"visibility\":\"private\"},"
       "{\"name\":\"P B\",\"path\":\"proj-b\",\"http_url_to_repo\":\"https://gitlab.com/g/proj-b.git\","
       "\"ssh_url_to_repo\":\"git@gitlab.com:g/proj-b.git\",\"visibility\":\"public\"}]";
   cJSON *out = cJSON_CreateArray();
   int n = git_org_parse(GIT_ORG_GITLAB, body, out);
   assert(n == 2);
   /* GitLab: name comes from `path`; clone_url from http_url_to_repo. */
   assert(strcmp(sval(repo_at(out, 0), "name"), "proj-a") == 0);
   assert(strcmp(sval(repo_at(out, 0), "clone_url"), "https://gitlab.com/g/proj-a.git") == 0);
   assert(cJSON_IsTrue(cJSON_GetObjectItem(repo_at(out, 0), "private")));  /* visibility=private */
   assert(cJSON_IsFalse(cJSON_GetObjectItem(repo_at(out, 1), "private"))); /* visibility=public  */
   cJSON_Delete(out);
   printf("  parse gitlab: OK\n");
}

static void test_parse_bitbucket(void)
{
   const char *body =
       "{\"values\":[{\"name\":\"bb-a\",\"is_private\":true,\"links\":{\"clone\":["
       "{\"name\":\"https\",\"href\":\"https://bitbucket.org/t/bb-a.git\"},"
       "{\"name\":\"ssh\",\"href\":\"git@bitbucket.org:t/bb-a.git\"}]}}]}";
   cJSON *out = cJSON_CreateArray();
   int n = git_org_parse(GIT_ORG_BITBUCKET, body, out);
   assert(n == 1);
   assert(strcmp(sval(repo_at(out, 0), "name"), "bb-a") == 0);
   assert(strcmp(sval(repo_at(out, 0), "clone_url"), "https://bitbucket.org/t/bb-a.git") == 0);
   assert(strcmp(sval(repo_at(out, 0), "ssh_url"), "git@bitbucket.org:t/bb-a.git") == 0);
   assert(cJSON_IsTrue(cJSON_GetObjectItem(repo_at(out, 0), "private")));
   cJSON_Delete(out);
   printf("  parse bitbucket: OK\n");
}

static void test_parse_bad_shape(void)
{
   cJSON *out = cJSON_CreateArray();
   assert(git_org_parse(GIT_ORG_GITHUB, "{\"not\":\"an array\"}", out) == -1);
   assert(git_org_parse(GIT_ORG_GITHUB, "not json", out) == -1);
   cJSON_Delete(out);
   printf("  parse bad shape: OK\n");
}

static void test_list_happy(void)
{
   g_page1_body =
       "[{\"name\":\"repo-a\",\"clone_url\":\"https://github.com/RakuenSoftware/repo-a.git\","
       "\"ssh_url\":\"git@github.com:RakuenSoftware/repo-a.git\",\"private\":false},"
       "{\"name\":\"repo-b\",\"clone_url\":\"https://github.com/RakuenSoftware/repo-b.git\","
       "\"ssh_url\":\"git@github.com:RakuenSoftware/repo-b.git\",\"private\":true}]";
   cJSON *out = NULL;
   char provider[32], err[256];
   int st = git_org_repos_list("github.com", "RakuenSoftware", &out, provider, sizeof(provider),
                               err, sizeof(err));
   assert(st == 0);
   assert(out && repo_count(out) == 2);
   assert(strcmp(provider, "github") == 0);
   /* Hit the org endpoint first. */
   assert(strstr(g_last_url, "https://api.github.com/orgs/RakuenSoftware/repos") != NULL);
   cJSON_Delete(out);
   printf("  list happy path: OK\n");
}

static void test_list_bad_args(void)
{
   cJSON *out = NULL;
   char provider[32], err[256];
   assert(git_org_repos_list("github.com", "bad owner!", &out, provider, sizeof(provider), err,
                             sizeof(err)) == 400);
   assert(out == NULL);
   assert(git_org_repos_list("", "owner", &out, provider, sizeof(provider), err, sizeof(err)) == 400);
   printf("  list bad args: OK\n");
}

int main(void)
{
   printf("test_git_org_repos:\n");
   test_detect();
   test_parse_github();
   test_parse_gitlab();
   test_parse_bitbucket();
   test_parse_bad_shape();
   test_list_happy();
   test_list_bad_args();
   printf("ALL PASS\n");
   return 0;
}
