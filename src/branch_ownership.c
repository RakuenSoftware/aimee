/* branch_ownership.c: per-session branch ownership for the MCP git tools.
 *
 * Records live behind the DB2 git ownership API. The operations here are
 * the only writers; mcp_git_* uses the guard helpers
 * (branch_own_guard, branch_own_guard_for) to gate writes on a branch the
 * current session does not own. Worktrees and the main checkout share one
 * ownership scope via --git-common-dir resolution in get_repo_path. */

#include <stdio.h>
#include "db1_client/git_ownership.h"
#include <stdlib.h>
#include <string.h>

#include "headers/aimee.h"
#include "headers/branch_ownership.h"
#include "config.h"
#include "headers/util.h"
#include "cJSON.h"

/* Provider-aware Git runner: unlike run_cmd(), this reaches a detached
 * workspace through its registered client-side runner. */
extern char *mcp_git_run(const char *cmd, int *exit_code);

/* --- Helpers --- */

static cJSON *mcp_text(const char *text)
{
   cJSON *arr = cJSON_CreateArray();
   cJSON *item = cJSON_CreateObject();
   cJSON_AddStringToObject(item, "type", "text");
   cJSON_AddStringToObject(item, "text", text);
   cJSON_AddItemToArray(arr, item);
   return arr;
}

static int get_current_branch(char *buf, size_t len)
{
   int rc;
   char *out = mcp_git_run("git rev-parse --abbrev-ref HEAD 2>/dev/null", &rc);
   if (rc != 0 || !out)
   {
      free(out);
      return -1;
   }
   char *nl = strchr(out, '\n');
   if (nl)
      *nl = '\0';
   snprintf(buf, len, "%s", out);
   free(out);
   return 0;
}

/* Get the canonical git repo root for the current cwd. Returns 0 on success.
 * In a worktree, --show-toplevel returns the worktree root, not the main repo.
 * We use --git-common-dir to find the shared .git dir and derive the main root,
 * so ownership records are consistent across main checkout and worktrees. */
int get_repo_path(char *buf, size_t len)
{
   int rc;

   /* Try --git-common-dir first: in a worktree this returns the absolute path
    * to the main repo's .git dir (e.g. "/root/dev/aimee/.git").
    * In a regular checkout it returns ".git" (relative). */
   char *common = mcp_git_run("git rev-parse --git-common-dir 2>/dev/null", &rc);
   if (rc == 0 && common)
   {
      char *nl = strchr(common, '\n');
      if (nl)
         *nl = '\0';

      if (strcmp(common, ".git") != 0 && common[0] == '/')
      {
         /* Absolute path like "/root/dev/aimee/.git" or
          * "/root/dev/aimee/.git/worktrees/..." — strip from /.git onward */
         char *git_suffix = strstr(common, "/.git");
         if (git_suffix)
         {
            *git_suffix = '\0';
            snprintf(buf, len, "%s", common);
            free(common);
            return 0;
         }
      }
      free(common);
   }
   else
   {
      free(common);
   }

   /* Fallback: regular checkout — use --show-toplevel */
   char *out = mcp_git_run("git rev-parse --show-toplevel 2>/dev/null", &rc);
   if (rc != 0 || !out)
   {
      free(out);
      return -1;
   }
   char *nl = strchr(out, '\n');
   if (nl)
      *nl = '\0';
   snprintf(buf, len, "%s", out);
   free(out);
   return 0;
}

/* --- Ownership operations --- */

int branch_own_register(const char *branch)
{
   char repo[MAX_PATH_LEN];
   if (get_repo_path(repo, sizeof(repo)) != 0)
      return -1;

   return db1_git_ownership_upsert(repo, branch, session_id());
}

void branch_own_delete(const char *branch)
{
   char repo[MAX_PATH_LEN];
   if (get_repo_path(repo, sizeof(repo)) != 0)
      return;
   (void)db1_git_ownership_delete(repo, branch);
}

int branch_own_check(const char *branch, char *owner_out, size_t owner_len)
{
   /* main/master are shared — never owned */
   if (strcmp(branch, "main") == 0 || strcmp(branch, "master") == 0)
      return 1;

   char repo[MAX_PATH_LEN];
   if (get_repo_path(repo, sizeof(repo)) != 0)
      return 1;

   int lookup = db1_git_ownership_get_owner(repo, branch, owner_out, owner_len);
   if (lookup < 0)
      return 1; /* no db / lookup failure = no enforcement */
   if (lookup == 1)
   {
      if (strcmp(owner_out, session_id()) != 0)
      {
         return 0; /* blocked */
      }
   }
   /* No owner recorded or owned by current session = allowed */
   return 1;
}

int mcp_git_branch_own_register(const char *repo_path, const char *branch)
{
   return db1_git_ownership_upsert(repo_path, branch, session_id());
}

int branch_own_get_session_branch(char *branch_out, size_t branch_len)
{
   char repo[MAX_PATH_LEN];
   if (get_repo_path(repo, sizeof(repo)) != 0)
      return -1;
   int lookup =
       db1_git_ownership_get_branch_for_session(repo, session_id(), branch_out, branch_len);
   return (lookup == 1) ? 0 : -1;
}

/* --- the feature branch this session's PRs target ---------------------------
 *
 * Recorded in db1 next to branch ownership, and for the same reason: this is read
 * by whichever process opens the PR, which is aimee-server. A server serving a
 * DETACHED or MIRROR workspace cannot see the checkout's own directory (that is
 * #2386: server-side path access against a client-held checkout, which broke every
 * pr create), so a file under the worktree is not a store the PR path can use.
 * repo_path comes from get_repo_path(), which resolves worktrees to the shared
 * --git-common-dir root, so every worktree of one repo agrees. */

/* 1 iff `branch` is a well-formed feature branch: the aimee/feat/ prefix followed by a
 * non-empty [a-z0-9-] slug, no leading or trailing dash, at most 40 chars. Validated
 * HERE, at the writer, because what reaches this function is caller-supplied: a PR's
 * `base` comes straight from an agent or an operator, and whatever is stored is what a
 * later PR in this session will be opened against. The charset excludes '/', '.',
 * whitespace and control characters, so a stored value cannot walk out of the
 * aimee/feat/ namespace or smuggle anything into a ref name. */
static int feature_branch_valid(const char *branch)
{
   static const char *PREFIX = "aimee/feat/";
   if (!branch || strncmp(branch, PREFIX, strlen(PREFIX)) != 0)
      return 0;
   const char *slug = branch + strlen(PREFIX);
   size_t len = strlen(slug);
   if (len == 0 || len > 40)
      return 0;
   if (slug[0] == '-' || slug[len - 1] == '-')
      return 0;
   for (size_t i = 0; i < len; i++)
   {
      char c = slug[i];
      if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'))
         return 0;
   }
   return 1;
}

int feature_branch_set(const char *branch)
{
   if (!feature_branch_valid(branch))
      return -1;
   char repo[MAX_PATH_LEN];
   if (get_repo_path(repo, sizeof(repo)) != 0)
      return -1;
   return db1_session_feature_branch_upsert(repo, session_id(), branch);
}

int feature_branch_for_session(char *branch_out, size_t branch_len)
{
   if (!branch_out || branch_len == 0)
      return -1;
   branch_out[0] = '\0';
   char repo[MAX_PATH_LEN];
   if (get_repo_path(repo, sizeof(repo)) != 0)
      return -1;
   /* db1 answers FOUND(1)/not-found(0)/error(-1); this function's callers want
    * 0 = use this branch, 1 = nothing named, -1 = could not tell. An error must NOT
    * collapse into "nothing named": the PR path treats that as "fall back to the
    * default branch", which would silently retarget a PR whenever the store was
    * unreachable. */
   int lookup = db1_session_feature_branch_get(repo, session_id(), branch_out, branch_len);
   if (lookup == 1)
      return 0;
   if (lookup == 0)
      return 1;
   return -1;
}

cJSON *branch_own_guard_for(const char *branch, const char *operation)
{
   char owner[64];
   if (!branch_own_check(branch, owner, sizeof(owner)))
   {
      char buf[512];
      snprintf(buf, sizeof(buf),
               "error: %s blocked — branch '%s' is owned by session %s. "
               "Use branch action=claim with force=true to transfer stale ownership.",
               operation, branch, owner);
      return mcp_text(buf);
   }
   return NULL; /* allowed */
}

cJSON *branch_own_guard(const char *operation)
{
   char branch[256];
   if (get_current_branch(branch, sizeof(branch)) != 0)
      return NULL; /* can't determine branch — allow */
   return branch_own_guard_for(branch, operation);
}
