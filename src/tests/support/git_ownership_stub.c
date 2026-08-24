/* git_ownership_stub.c: branch ownership, kept per repo, in memory.
 *
 * test_mcp_git asserts that claiming the same branch name in two different
 * repositories records two different owners:
 *
 *    claim "some-branch" as session-C in repo A
 *    claim "some-branch" as session-D in repo B
 *    assert A reports session-C and B reports session-D
 *
 * The subject there is the MCP GIT TOOL -- that a claim is scoped to its
 * repository rather than to the branch name -- and the store is only where the
 * answer is read back from. So this keys on (repo, branch): a stub keyed on
 * branch alone would answer one owner for both and quietly retire the only
 * assertion that distinguishes them, while leaving it in the file.
 *
 * The real functions are bus clients now, so linking them would make a test
 * about argument handling need a running store module.
 *
 * Reads follow the header's convention: FOUND(1), not-found(0), error(-1).
 */

#include <stdio.h>
#include <string.h>

#include "db1_client/git_ownership.h"

#define STUB_MAX 16
#define STUB_STR 256

static struct
{
   char repo[STUB_STR];
   char branch[STUB_STR];
   char session[STUB_STR];
   int used;
} g_rows[STUB_MAX];

static int find(const char *repo, const char *branch)
{
   for (int i = 0; i < STUB_MAX; i++)
   {
      if (g_rows[i].used && strcmp(g_rows[i].repo, repo) == 0 &&
          strcmp(g_rows[i].branch, branch) == 0)
      {
         return i;
      }
   }
   return -1;
}

int db1_git_ownership_upsert(const char *repo_path, const char *branch_name, const char *session_id)
{
   if (!repo_path || !branch_name || !session_id)
   {
      return -1;
   }
   int i = find(repo_path, branch_name);
   if (i < 0)
   {
      for (i = 0; i < STUB_MAX && g_rows[i].used; i++)
      {
      }
      if (i == STUB_MAX)
      {
         /* Louder than a silent overwrite: a test that outgrows the table
          * should fail here rather than start losing claims. */
         return -1;
      }
      snprintf(g_rows[i].repo, STUB_STR, "%s", repo_path);
      snprintf(g_rows[i].branch, STUB_STR, "%s", branch_name);
      g_rows[i].used = 1;
   }
   snprintf(g_rows[i].session, STUB_STR, "%s", session_id);
   return 0;
}

int db1_git_ownership_delete(const char *repo_path, const char *branch_name)
{
   if (!repo_path || !branch_name)
   {
      return -1;
   }
   int i = find(repo_path, branch_name);
   if (i >= 0)
   {
      g_rows[i].used = 0;
   }
   return 0;
}

int db1_git_ownership_get_owner(const char *repo_path, const char *branch_name, char *owner_out,
                                size_t owner_len)
{
   if (!repo_path || !branch_name || !owner_out || owner_len == 0)
   {
      return -1;
   }
   int i = find(repo_path, branch_name);
   if (i < 0)
   {
      owner_out[0] = '\0';
      return 0;
   }
   snprintf(owner_out, owner_len, "%s", g_rows[i].session);
   return 1;
}

/* The reverse lookup: which branch this session claimed in this repo. Same
 * rows, read the other way, so a claim made through the tool is visible to the
 * code asking "what is this session working on here". */
int db1_git_ownership_get_branch_for_session(const char *repo_path, const char *session_id,
                                             char *branch_out, size_t branch_len)
{
   if (!repo_path || !session_id || !branch_out || branch_len == 0)
   {
      return -1;
   }
   for (int i = 0; i < STUB_MAX; i++)
   {
      if (g_rows[i].used && strcmp(g_rows[i].repo, repo_path) == 0 &&
          strcmp(g_rows[i].session, session_id) == 0)
      {
         snprintf(branch_out, branch_len, "%s", g_rows[i].branch);
         return 1;
      }
   }
   branch_out[0] = '\0';
   return 0;
}

/* Prefix lookup across every repo. AMBIGUOUS PREFIXES REPORT NOT-FOUND rather
 * than picking the first row: a prefix matching two sessions has no answer, and
 * returning one of them would let a resolution test pass on row order. */
int db1_git_ownership_find_session_by_prefix(const char *session_prefix, char *session_out,
                                             size_t session_len)
{
   if (!session_prefix || !session_out || session_len == 0)
   {
      return -1;
   }
   const size_t n = strlen(session_prefix);
   const char *hit = NULL;
   for (int i = 0; i < STUB_MAX; i++)
   {
      if (!g_rows[i].used || strncmp(g_rows[i].session, session_prefix, n) != 0)
      {
         continue;
      }
      if (hit && strcmp(hit, g_rows[i].session) != 0)
      {
         session_out[0] = '\0';
         return 0;
      }
      hit = g_rows[i].session;
   }
   if (!hit)
   {
      session_out[0] = '\0';
      return 0;
   }
   snprintf(session_out, session_len, "%s", hit);
   return 1;
}

/* --- the feature branch a session's PRs target ------------------------------
 *
 * Keyed on (repo, session) like ownership, and kept in its own table because it
 * is a different fact: ownership says which branch a session claimed, this says
 * which branch its pull requests aim at. A test can set one without the other.
 */

static struct
{
   char repo[STUB_STR];
   char session[STUB_STR];
   char branch[STUB_STR];
   int used;
} g_feature[STUB_MAX];

int db1_session_feature_branch_upsert(const char *repo_path, const char *session_id,
                                      const char *feature_branch)
{
   if (!repo_path || !session_id || !feature_branch)
   {
      return -1;
   }
   int free_slot = -1;
   for (int i = 0; i < STUB_MAX; i++)
   {
      if (g_feature[i].used && strcmp(g_feature[i].repo, repo_path) == 0 &&
          strcmp(g_feature[i].session, session_id) == 0)
      {
         snprintf(g_feature[i].branch, STUB_STR, "%s", feature_branch);
         return 0;
      }
      if (!g_feature[i].used && free_slot < 0)
      {
         free_slot = i;
      }
   }
   if (free_slot < 0)
   {
      return -1;
   }
   snprintf(g_feature[free_slot].repo, STUB_STR, "%s", repo_path);
   snprintf(g_feature[free_slot].session, STUB_STR, "%s", session_id);
   snprintf(g_feature[free_slot].branch, STUB_STR, "%s", feature_branch);
   g_feature[free_slot].used = 1;
   return 0;
}

int db1_session_feature_branch_get(const char *repo_path, const char *session_id, char *branch_out,
                                   size_t branch_len)
{
   if (!repo_path || !session_id || !branch_out || branch_len == 0)
   {
      return -1;
   }
   for (int i = 0; i < STUB_MAX; i++)
   {
      if (g_feature[i].used && strcmp(g_feature[i].repo, repo_path) == 0 &&
          strcmp(g_feature[i].session, session_id) == 0)
      {
         snprintf(branch_out, branch_len, "%s", g_feature[i].branch);
         return 1;
      }
   }
   branch_out[0] = '\0';
   return 0;
}
