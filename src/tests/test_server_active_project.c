/* Unit tests for the active-project root matcher.
 *
 * Only the path-matching half is exercised here: it is pure, and it is the part
 * that answers for a remote thin client whose `cwd` this process cannot stat.
 * The filesystem-identity half is covered by the workspace tests. */
#include "index.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

int server_active_project_match(const char *cwd, const project_info_t *projects, int count,
                                char *out, size_t outlen);

int server_active_project_from_cwd(const char *cwd, char *out, size_t outlen);

/* server_active_project.c's other half calls into the workspace and knowledge
 * modules, which are not in aimee-core. Drive both from here so the resolution
 * order itself is testable: what the working tree claims, and what is actually
 * indexed, are exactly the two inputs whose disagreement caused the bug. */
static const char *g_identity;         /* NULL => the working tree cannot answer */
static const project_info_t *g_listed; /* what kb_client_index_list reports */
static int g_listed_count;             /* negative => the list is unavailable */

int workspace_repo_identity(const char *cwd, char *project_out, size_t project_len,
                            char *workspace_out, size_t workspace_len)
{
   (void)cwd;
   (void)workspace_out;
   (void)workspace_len;
   if (project_out && project_len)
      project_out[0] = '\0';
   if (!g_identity)
      return -1;
   snprintf(project_out, project_len, "%s", g_identity);
   return 0;
}

int kb_client_index_list(project_info_t *out, int max)
{
   if (g_listed_count <= 0)
      return g_listed_count;
   int n = g_listed_count < max ? g_listed_count : max;
   for (int i = 0; i < n; i++)
      out[i] = g_listed[i];
   return n;
}

static project_info_t mk(const char *name, const char *root)
{
   project_info_t p;
   memset(&p, 0, sizeof(p));
   snprintf(p.name, sizeof(p.name), "%s", name);
   snprintf(p.root, sizeof(p.root), "%s", root);
   return p;
}

int main(void)
{
   char out[128];

   /* Exact root match. */
   {
      project_info_t projects[] = {mk("alpha", "/srv/alpha")};
      assert(server_active_project_match("/srv/alpha", projects, 1, out, sizeof(out)) == 0);
      assert(strcmp(out, "alpha") == 0);
   }

   /* A cwd nested under the root resolves to that project. */
   {
      project_info_t projects[] = {mk("alpha", "/srv/alpha")};
      assert(server_active_project_match("/srv/alpha/app/sub", projects, 1, out, sizeof(out)) == 0);
      assert(strcmp(out, "alpha") == 0);
   }

   /* Longest matching root wins, so a nested workspace beats its parent —
    * regardless of the order the knowledge service returns them in. */
   {
      project_info_t projects[] = {mk("outer", "/srv/alpha"), mk("inner", "/srv/alpha/inner")};
      assert(server_active_project_match("/srv/alpha/inner/x", projects, 2, out, sizeof(out)) == 0);
      assert(strcmp(out, "inner") == 0);

      project_info_t reversed[] = {mk("inner", "/srv/alpha/inner"), mk("outer", "/srv/alpha")};
      assert(server_active_project_match("/srv/alpha/inner/x", reversed, 2, out, sizeof(out)) == 0);
      assert(strcmp(out, "inner") == 0);
   }

   /* Prefix matching is component-aware: /srv/foobar must not claim /srv/foo. */
   {
      project_info_t projects[] = {mk("foobar", "/srv/foobar")};
      assert(server_active_project_match("/srv/foo", projects, 1, out, sizeof(out)) == -1);
      assert(out[0] == '\0');
   }

   /* A trailing slash on the stored root does not defeat the boundary test. */
   {
      project_info_t projects[] = {mk("alpha", "/srv/alpha/")};
      assert(server_active_project_match("/srv/alpha/app", projects, 1, out, sizeof(out)) == 0);
      assert(strcmp(out, "alpha") == 0);
      assert(server_active_project_match("/srv/alphaX", projects, 1, out, sizeof(out)) == -1);
   }

   /* No covering root: the caller must fall through to `scope_required`. */
   {
      project_info_t projects[] = {mk("alpha", "/srv/alpha")};
      assert(server_active_project_match("/home/other/repo", projects, 1, out, sizeof(out)) == -1);
      assert(out[0] == '\0');
   }

   /* Degenerate inputs are refusals, never a stale or partial project name. */
   {
      project_info_t projects[] = {mk("alpha", "/srv/alpha")};
      assert(server_active_project_match(NULL, projects, 1, out, sizeof(out)) == -1);
      assert(server_active_project_match("", projects, 1, out, sizeof(out)) == -1);
      assert(server_active_project_match("/srv/alpha", NULL, 0, out, sizeof(out)) == -1);
      assert(server_active_project_match("/srv/alpha", projects, 0, out, sizeof(out)) == -1);
   }

   /* A project row with no name is skipped rather than returned empty. */
   {
      project_info_t projects[] = {mk("", "/srv/alpha")};
      assert(server_active_project_match("/srv/alpha", projects, 1, out, sizeof(out)) == -1);
      assert(out[0] == '\0');
   }

   /* ---- resolution order: the working tree vs what is actually indexed ----
    *
    * THE BUG: `index scan <name> <root>` names the project, while this
    * resolution returns the repo's persisted identity — a UUID when the repo has
    * no remote. Nothing makes those agree. A freshly scanned workspace resolved
    * to the UUID, queried a project that does not exist, and answered
    * "code index lookup failed", while the identical lookup with an explicit
    * project name returned hits. */
   {
      project_info_t indexed[] = {mk("settle", "/srv/settle")};

      /* An identity that names a real project still wins: it is the durable one,
       * and it is what makes a repo resolve to the same project from any host. */
      g_identity = "settle";
      g_listed = indexed;
      g_listed_count = 1;
      assert(server_active_project_from_cwd("/srv/settle", out, sizeof(out)) == 0);
      assert(strcmp(out, "settle") == 0);

      /* THE REGRESSION TEST: the identity names nothing indexed, so fall through
       * to the registered root rather than returning a project that cannot
       * answer. */
      g_identity = "3014e417-337b-444f-9aaf-432838cdcf82";
      assert(server_active_project_from_cwd("/srv/settle/app", out, sizeof(out)) == 0);
      assert(strcmp(out, "settle") == 0);

      /* No identity at all (the remote-client case) still matches on root. */
      g_identity = NULL;
      assert(server_active_project_from_cwd("/srv/settle", out, sizeof(out)) == 0);
      assert(strcmp(out, "settle") == 0);

      /* Neither source can answer: refuse, so the caller emits scope_required. */
      g_identity = NULL;
      g_listed_count = 0;
      assert(server_active_project_from_cwd("/srv/settle", out, sizeof(out)) == -1);
      assert(out[0] == '\0');

      /* The list is UNAVAILABLE rather than empty. An unverified identity is
       * better than failing a lookup that worked before the check existed. */
      g_identity = "settle";
      g_listed_count = -1;
      assert(server_active_project_from_cwd("/srv/settle", out, sizeof(out)) == 0);
      assert(strcmp(out, "settle") == 0);
   }

   printf("test_server_active_project: all assertions passed\n");
   return 0;
}
