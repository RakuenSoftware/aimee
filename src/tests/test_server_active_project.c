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

/* server_active_project.c's other half calls into the workspace and knowledge
 * modules, which are not in aimee-core. Stub them to the remote-topology answer
 * — the working tree is unreadable here and no projects are listed — so this
 * binary exercises the matcher alone. */
int workspace_repo_identity(const char *cwd, char *project_out, size_t project_len,
                            char *workspace_out, size_t workspace_len)
{
   (void)cwd;
   (void)workspace_out;
   (void)workspace_len;
   if (project_out && project_len)
      project_out[0] = '\0';
   return -1;
}

int kb_client_index_list(project_info_t *out, int max)
{
   (void)out;
   (void)max;
   return 0;
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

   printf("test_server_active_project: all assertions passed\n");
   return 0;
}
