/* test_util_url.c: unit tests for the canonical URL pipeline. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "util_url.h"

#define PASS(name) printf("  PASS: %s\n", name)

static void expect_normalize(const char *input, const char *expected)
{
   char *got = util_url_normalize(input);
   if (expected == NULL)
   {
      assert(got == NULL);
   }
   else
   {
      if (got == NULL)
      {
         fprintf(stderr, "normalize(%s): expected %s, got NULL\n", input, expected);
         abort();
      }
      if (strcmp(got, expected) != 0)
      {
         fprintf(stderr, "normalize(%s): expected %s, got %s\n", input, expected, got);
         abort();
      }
   }
   free(got);
}

static void expect_parent(const char *input, const char *expected)
{
   char *got = util_url_workspace_parent(input);
   if (expected == NULL)
   {
      assert(got == NULL);
   }
   else
   {
      assert(got != NULL);
      if (strcmp(got, expected) != 0)
      {
         fprintf(stderr, "workspace_parent(%s): expected %s, got %s\n", input, expected, got);
         abort();
      }
   }
   free(got);
}

static void test_transport_rewrite(void)
{
   /* scp-like */
   expect_normalize("git@github.com:org/repo.git", "https://github.com/org/repo");
   expect_normalize("git@github.com:org/repo", "https://github.com/org/repo");
   /* ssh:// */
   expect_normalize("ssh://git@github.com/org/repo.git", "https://github.com/org/repo");
   expect_normalize("ssh://git@github.com:22/org/repo.git", "https://github.com/org/repo");
   /* git:// */
   expect_normalize("git://github.com/org/repo.git", "https://github.com/org/repo");
   /* http → https */
   expect_normalize("http://github.com/org/repo.git", "https://github.com/org/repo");
   /* https passthrough */
   expect_normalize("https://github.com/org/repo", "https://github.com/org/repo");
   PASS("transport_rewrite");
}

static void test_path_sanitization(void)
{
   /* trailing .git */
   expect_normalize("https://github.com/org/repo.git", "https://github.com/org/repo");
   /* trailing slash */
   expect_normalize("https://github.com/org/repo/", "https://github.com/org/repo");
   /* both */
   expect_normalize("https://github.com/org/repo.git/", "https://github.com/org/repo");
   /* double slashes collapsed */
   expect_normalize("https://github.com//org//repo.git", "https://github.com/org/repo");
   /* leading slash run after host */
   expect_normalize("https://github.com///org/repo", "https://github.com/org/repo");
   PASS("path_sanitization");
}

static void test_case_normalization(void)
{
   /* Scheme and host always lowercased */
   expect_normalize("HTTPS://GITHUB.COM/Org/Repo", "https://github.com/org/repo");
   /* github.com: path lowercased */
   expect_normalize("https://github.com/Games-On-Whales/Wolf.git",
                    "https://github.com/games-on-whales/wolf");
   /* gitlab.com: path lowercased */
   expect_normalize("https://GitLab.com/Group/SubGroup/Repo",
                    "https://gitlab.com/group/subgroup/repo");
   /* bitbucket.org: path lowercased */
   expect_normalize("https://BITBUCKET.ORG/Team/Repo", "https://bitbucket.org/team/repo");
   /* well-known subdomain: path lowercased */
   expect_normalize("https://gist.github.com/User/Abcd", "https://gist.github.com/user/abcd");
   /* unknown host: path preserved */
   expect_normalize("https://gitea.example.com/Team/Project.git",
                    "https://gitea.example.com/Team/Project");
   expect_normalize("git@gitea.example.com:Team/Project.git",
                    "https://gitea.example.com/Team/Project");
   /* enterprise github on a non-github.com host: treated as unknown, path preserved */
   expect_normalize("https://github.enterprise.example.com/Org/Repo",
                    "https://github.enterprise.example.com/Org/Repo");
   PASS("case_normalization");
}

static void test_invalid_inputs(void)
{
   expect_normalize(NULL, NULL);
   expect_normalize("", NULL);
   /* Unknown scheme */
   expect_normalize("file:///tmp/repo", NULL);
   expect_normalize("rsync://host/path", NULL);
   /* scp-like without user@ */
   expect_normalize("github.com:org/repo", NULL);
   /* missing host */
   expect_normalize("https:///org/repo", NULL);
   /* missing path */
   expect_normalize("https://github.com", NULL);
   expect_normalize("https://github.com/", NULL);
   /* just a scheme */
   expect_normalize("https://", NULL);
   PASS("invalid_inputs");
}

static void test_workspace_parent(void)
{
   /* github repo → org workspace */
   expect_parent("https://github.com/games-on-whales/wolf", "https://github.com/games-on-whales");
   /* gitlab subgroup walk */
   char *p1 = util_url_workspace_parent("https://gitlab.com/group/subgroup/repo");
   assert(p1 && strcmp(p1, "https://gitlab.com/group/subgroup") == 0);
   char *p2 = util_url_workspace_parent(p1);
   assert(p2 && strcmp(p2, "https://gitlab.com/group") == 0);
   char *p3 = util_url_workspace_parent(p2);
   assert(p3 == NULL); /* already at host root */
   free(p1);
   free(p2);
   free(p3);
   /* already at workspace root → NULL */
   expect_parent("https://github.com/org", NULL);
   /* local scope → NULL */
   expect_parent("local:01928374-5678-7abc-def0-123456789abc", NULL);
   /* NULL → NULL */
   expect_parent(NULL, NULL);
   /* non-https → NULL */
   expect_parent("ssh://git@github.com/org/repo", NULL);
   PASS("workspace_parent");
}

static void test_host_case_classifier(void)
{
   assert(util_url_host_is_case_insensitive("github.com") == 1);
   assert(util_url_host_is_case_insensitive("gitlab.com") == 1);
   assert(util_url_host_is_case_insensitive("bitbucket.org") == 1);
   assert(util_url_host_is_case_insensitive("gist.github.com") == 1);
   assert(util_url_host_is_case_insensitive("api.gitlab.com") == 1);
   assert(util_url_host_is_case_insensitive("gitea.example.com") == 0);
   assert(util_url_host_is_case_insensitive("") == 0);
   assert(util_url_host_is_case_insensitive(NULL) == 0);
   /* Must not match by substring — "github.com.attacker.example" should be case-sensitive. */
   assert(util_url_host_is_case_insensitive("github.com.attacker.example") == 0);
   PASS("host_case_classifier");
}

static void test_is_ssh(void)
{
   /* SSH transports (key auth): ssh:// and scp-like user@host:path. */
   assert(util_url_is_ssh("git@bitbucket.org:team/repo.git") == 1);
   assert(util_url_is_ssh("ssh://git@github.com/o/r.git") == 1);
   assert(util_url_is_ssh("ssh://git@ssh.dev.azure.com:22/v3/o/p/r") == 1);
   assert(util_url_is_ssh("user@host.example:some/deep/path") == 1);
   /* Not SSH: https/http, git:// (anonymous, no key/host key), local paths. */
   assert(util_url_is_ssh("https://github.com/o/r.git") == 0);
   assert(util_url_is_ssh("http://host/o/r") == 0);
   assert(util_url_is_ssh("git://github.com/o/r.git") == 0);
   assert(util_url_is_ssh("/srv/repos/local.git") == 0);
   assert(util_url_is_ssh("relative/path") == 0);
   /* A ':' with no '@' before it is not scp-like (e.g. a host:port with scheme). */
   assert(util_url_is_ssh("host:1234/path") == 0);
   /* Scheme match is case-insensitive; git:// stays non-SSH regardless of case. */
   assert(util_url_is_ssh("SSH://git@github.com/o/r") == 1);
   assert(util_url_is_ssh("Ssh://git@github.com/o/r.git") == 1);
   assert(util_url_is_ssh("GIT://github.com/o/r") == 0);
   assert(util_url_is_ssh("") == 0);
   assert(util_url_is_ssh(NULL) == 0);
   PASS("is_ssh");
}

int main(void)
{
   printf("Running util_url tests\n");
   test_transport_rewrite();
   test_path_sanitization();
   test_case_normalization();
   test_invalid_inputs();
   test_workspace_parent();
   test_host_case_classifier();
   test_is_ssh();
   printf("All util_url tests passed.\n");
   return 0;
}
