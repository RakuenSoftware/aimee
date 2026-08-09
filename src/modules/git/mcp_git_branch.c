/* mcp_git_branch.c: MCP git branch, stash, tag, and fetch handlers */
#include "aimee.h"
#include "cJSON.h"
#include "git_verify.h"
#include "mcp_git.h"
#include "util.h"
#include "branch_ownership.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static cJSON *mcp_error(const char *fmt, const char *detail)
{
   char buf[1024];
   snprintf(buf, sizeof(buf), fmt, detail);
   return mcp_text(buf);
}

/* --- git_branch --- */

cJSON *handle_git_branch(cJSON *args)
{
   cJSON *jaction = cJSON_GetObjectItemCaseSensitive(args, "action");
   if (!cJSON_IsString(jaction))
      return mcp_text(
          "error: 'action' parameter is required (create/switch/list/delete/claim/release)");

   const char *action = jaction->valuestring;
   cJSON *jname = cJSON_GetObjectItemCaseSensitive(args, "name");

   if (strcmp(action, "list") == 0)
   {
      int rc;
      char *out = mcp_git_run("git branch -v --format='%(if)%(HEAD)%(then)* %(else)  "
                              "%(end)%(refname:short) (%(objectname:short))' 2>&1",
                              &rc);
      if (rc != 0)
      {
         cJSON *r = mcp_error("error: git branch failed: %s", out ? out : "unknown");
         free(out);
         return r;
      }
      /* Truncate to reasonable length */
      if (out && strlen(out) > 2048)
      {
         const char *suffix = "\n... (truncated)";
         size_t slen = strlen(suffix);
         memcpy(out + 2048, suffix, slen + 1);
      }
      cJSON *r = mcp_text(out ? out : "(no branches)");
      free(out);
      return r;
   }

   if (!cJSON_IsString(jname) || !jname->valuestring[0])
      return mcp_text("error: 'name' parameter is required for create/switch/delete/claim/release");

   char *esc_name = shell_escape(jname->valuestring);

   /* Main branch protection: block deleting main/master */
   if (strcmp(action, "delete") == 0 &&
       (strcmp(jname->valuestring, "main") == 0 || strcmp(jname->valuestring, "master") == 0))
   {
      free(esc_name);
      char buf[512];
      snprintf(buf, sizeof(buf),
               "error: %s '%s' blocked — writing to the main branch is not allowed. "
               "Create a feature branch first.",
               action, jname->valuestring);
      return mcp_text(buf);
   }

   if (strcmp(action, "create") == 0)
   {
      cJSON *jbase = cJSON_GetObjectItemCaseSensitive(args, "base");
      char cmd[1024];

      if (mcp_git_get_worktree())
      {
         /* In worktree: create branch WITHOUT switching (git branch, not git checkout -b).
          * Switching would detach the worktree from its session branch. */
         if (cJSON_IsString(jbase) && jbase->valuestring[0])
         {
            char *esc_base = shell_escape(jbase->valuestring);
            snprintf(cmd, sizeof(cmd), "git branch '%s' '%s' 2>&1", esc_name, esc_base);
            free(esc_base);
         }
         else
         {
            snprintf(cmd, sizeof(cmd), "git branch '%s' 2>&1", esc_name);
         }
      }
      else
      {
         if (cJSON_IsString(jbase) && jbase->valuestring[0])
         {
            char *esc_base = shell_escape(jbase->valuestring);
            snprintf(cmd, sizeof(cmd), "git checkout -b '%s' '%s' 2>&1", esc_name, esc_base);
            free(esc_base);
         }
         else
         {
            snprintf(cmd, sizeof(cmd), "git checkout -b '%s' 2>&1", esc_name);
         }
      }

      int rc;
      char *out = mcp_git_run(cmd, &rc);
      if (rc != 0)
      {
         cJSON *r = mcp_error("error: git branch create failed: %s", out ? out : "unknown");
         free(out);
         free(esc_name);
         return r;
      }
      free(out);

      /* Get current commit hash */
      char *hash_out = mcp_git_run("git rev-parse --short HEAD 2>&1", &rc);
      char hash[16] = "";
      if (hash_out)
      {
         char *nl = strchr(hash_out, '\n');
         if (nl)
            *nl = '\0';
         snprintf(hash, sizeof(hash), "%s", hash_out);
         free(hash_out);
      }

      /* Register branch ownership for this session */
      branch_own_register(jname->valuestring);

      char result[512];
      if (mcp_git_get_worktree())
         snprintf(result, sizeof(result),
                  "created: %s (%s)\n(worktree mode: staying on session branch)\nowner: %s",
                  jname->valuestring, hash, session_id());
      else
         snprintf(result, sizeof(result), "created: %s (%s)\nswitched to %s\nowner: %s",
                  jname->valuestring, hash, jname->valuestring, session_id());
      free(esc_name);
      return mcp_text(result);
   }

   if (strcmp(action, "switch") == 0)
   {
      /* Block branch switching in worktrees — worktrees are session-scoped */
      if (mcp_git_get_worktree())
      {
         free(esc_name);
         return mcp_text(
             "error: branch switching is not allowed in a session worktree. "
             "Worktrees are locked to their session branch. Use git_branch action=create "
             "to create a new branch (it will be created without switching to it).");
      }

      /* Prefer an existing local branch. If it does not exist but origin has a
       * branch of that name, create the local branch explicitly from the
       * remote-tracking ref and set its upstream. Do not rely on checkout's
       * DWIM heuristic: its result changes when another remote is added. */
      const char *requested = jname->valuestring;
      const char *local_name = requested;
      if (strncmp(requested, "refs/remotes/origin/", 20) == 0)
         local_name = requested + 20;
      else if (strncmp(requested, "refs/heads/", 11) == 0)
         local_name = requested + 11;
      else if (strncmp(requested, "origin/", 7) == 0)
         local_name = requested + 7;
      if (!local_name[0])
      {
         free(esc_name);
         return mcp_text("error: branch name is required after origin/");
      }
      char *esc_local = shell_escape(local_name);
      free(esc_name);
      char probe[768];
      int rc = 0;
      snprintf(probe, sizeof(probe), "git show-ref --verify --quiet 'refs/heads/%s' 2>/dev/null",
               esc_local);
      char *probe_out = mcp_git_run(probe, &rc);
      free(probe_out);
      int local_exists = rc == 0;

      int tracking_origin = 0;
      if (!local_exists)
      {
         snprintf(probe, sizeof(probe),
                  "git show-ref --verify --quiet 'refs/remotes/origin/%s' 2>/dev/null", esc_local);
         probe_out = mcp_git_run(probe, &rc);
         free(probe_out);
         tracking_origin = rc == 0;
      }

      char cmd[1024];
      if (tracking_origin)
         snprintf(cmd, sizeof(cmd), "git checkout -b '%s' --track 'origin/%s' 2>&1", esc_local,
                  esc_local);
      else
         snprintf(cmd, sizeof(cmd), "git checkout '%s' 2>&1", esc_local);
      char *out = mcp_git_run(cmd, &rc);
      if (rc != 0)
      {
         cJSON *r = mcp_error("error: git switch failed: %s", out ? out : "unknown");
         free(out);
         free(esc_local);
         return r;
      }
      free(out);

      int ownership_ok = !tracking_origin || branch_own_register(local_name) == 0;

      char result[512];
      if (tracking_origin)
         snprintf(result, sizeof(result), "switched to %s (tracking origin/%s)%s", local_name,
                  local_name,
                  ownership_ok ? "" : "\nwarning: branch ownership could not be recorded");
      else
         snprintf(result, sizeof(result), "switched to %s", local_name);
      free(esc_local);
      return mcp_text(result);
   }

   if (strcmp(action, "orphan") == 0)
   {
      char cmd[512];
      snprintf(cmd, sizeof(cmd), "git checkout --orphan '%s' 2>&1", esc_name);
      int rc;
      char *out = mcp_git_run(cmd, &rc);
      if (rc != 0)
      {
         cJSON *r = mcp_error("error: git checkout --orphan failed: %s", out ? out : "unknown");
         free(out);
         free(esc_name);
         return r;
      }
      free(out);

      /* Register branch ownership for this session */
      branch_own_register(jname->valuestring);

      char result[512];
      snprintf(result, sizeof(result),
               "created orphan branch: %s\n"
               "All files are staged. Commit to create the root commit.\n"
               "owner: %s",
               jname->valuestring, session_id());
      free(esc_name);
      return mcp_text(result);
   }

   if (strcmp(action, "delete") == 0)
   {
      cJSON *jforce_del = cJSON_GetObjectItemCaseSensitive(args, "force");
      int force_del = (jforce_del && cJSON_IsTrue(jforce_del)) ? 1 : 0;
      cJSON *jremote = cJSON_GetObjectItemCaseSensitive(args, "remote");
      int remote = (jremote && cJSON_IsTrue(jremote)) ? 1 : 0;

      char cmd[512];
      snprintf(cmd, sizeof(cmd), "git branch %s '%s' 2>&1", force_del ? "-D" : "-d", esc_name);
      int rc;
      char *out = mcp_git_run(cmd, &rc);
      if (rc != 0)
      {
         cJSON *r = mcp_error("error: git branch delete failed: %s", out ? out : "unknown");
         free(out);
         free(esc_name);
         return r;
      }
      free(out);

      /* Also delete remote branch if requested */
      char remote_result[256] = "";
      if (remote)
      {
         char rcmd[512];
         snprintf(rcmd, sizeof(rcmd), "git push origin --delete '%s' 2>&1", esc_name);
         char *rout = mcp_git_run(rcmd, &rc);
         if (rc != 0)
            snprintf(remote_result, sizeof(remote_result), "\nremote delete failed: %s",
                     rout ? rout : "unknown");
         else
            snprintf(remote_result, sizeof(remote_result), "\nremote branch deleted");
         free(rout);
      }

      /* Clean up ownership record */
      branch_own_delete(jname->valuestring);

      char result[512];
      snprintf(result, sizeof(result), "deleted: %s%s", jname->valuestring, remote_result);
      free(esc_name);
      return mcp_text(result);
   }

   if (strcmp(action, "claim") == 0)
   {
      if (strcmp(jname->valuestring, "main") == 0 || strcmp(jname->valuestring, "master") == 0)
      {
         free(esc_name);
         return mcp_text("error: cannot claim main/master branches");
      }
      char owner[64];
      cJSON *jforce = cJSON_GetObjectItemCaseSensitive(args, "force");
      int force = cJSON_IsTrue(jforce);
      if (!branch_own_check(jname->valuestring, owner, sizeof(owner)) && !force)
      {
         char err[512];
         snprintf(
             err, sizeof(err),
             "error: branch '%s' is owned by session %s. "
             "Use branch action=claim with force=true to transfer ownership, or action=release "
             "from the owning session.",
             jname->valuestring, owner);
         free(esc_name);
         return mcp_text(err);
      }
      if (branch_own_register(jname->valuestring) != 0)
      {
         free(esc_name);
         return mcp_text("error: failed to register branch ownership");
      }
      char result[256];
      snprintf(result, sizeof(result), "claimed: %s (owner: %s)", jname->valuestring, session_id());
      free(esc_name);
      return mcp_text(result);
   }

   if (strcmp(action, "release") == 0)
   {
      char owner[64] = "";
      cJSON *jforce = cJSON_GetObjectItemCaseSensitive(args, "force");
      int force = cJSON_IsTrue(jforce);
      if (!branch_own_check(jname->valuestring, owner, sizeof(owner)) && !force)
      {
         char err[512];
         snprintf(err, sizeof(err),
                  "error: branch '%s' is owned by session %s. "
                  "Use force=true to release another session's stale ownership record.",
                  jname->valuestring, owner);
         free(esc_name);
         return mcp_text(err);
      }
      branch_own_delete(jname->valuestring);
      char result[256];
      snprintf(result, sizeof(result), "released ownership: %s%s", jname->valuestring,
               force ? " (forced)" : "");
      free(esc_name);
      return mcp_text(result);
   }

   free(esc_name);
   return mcp_text("error: unknown action. Use create/switch/list/delete/claim/release");
}

/* --- git_stash --- */

/* Find the stash index for an aimee auto-stash belonging to this session.
 * Returns the stash index (>= 0) or -1 if not found. */
static int find_session_stash(const char *sid)
{
   int rc;
   char *list = mcp_git_run("git stash list --format='%gd %s' 2>/dev/null", &rc);
   if (rc != 0 || !list || !list[0])
   {
      free(list);
      return -1;
   }
   char needle[64];
   snprintf(needle, sizeof(needle), "aimee-autostash-%.8s", sid);
   int found = -1;
   for (char *line = list; line && *line; line = line ? line + 1 : NULL)
   {
      char *nl = strchr(line, '\n');
      if (nl)
         *nl = '\0';
      if (strstr(line, needle))
      {
         const char *p = strchr(line, '{');
         if (p)
            found = atoi(p + 1);
         break;
      }
      line = nl;
   }
   free(list);
   return found;
}

cJSON *handle_git_stash(cJSON *args)
{
   cJSON *jaction = cJSON_GetObjectItemCaseSensitive(args, "action");
   const char *action =
       (cJSON_IsString(jaction) && jaction->valuestring[0]) ? jaction->valuestring : "push";

   int rc;
   char cmd[1024];

   if (strcmp(action, "push") == 0)
   {
      cJSON *jmsg = cJSON_GetObjectItemCaseSensitive(args, "message");
      if (cJSON_IsString(jmsg) && jmsg->valuestring[0])
      {
         char *esc = shell_escape(jmsg->valuestring);
         snprintf(cmd, sizeof(cmd), "git stash push -m '%s' 2>&1", esc);
         free(esc);
      }
      else
      {
         snprintf(cmd, sizeof(cmd), "git stash push 2>&1");
      }
   }
   else if (strcmp(action, "pop") == 0)
   {
      /* Session-aware pop: if this session has an auto-stash, pop that
       * specific entry instead of blindly popping stash@{0} which may
       * belong to another session. */
      const char *sid = session_id();
      int idx = find_session_stash(sid);
      if (idx >= 0)
         snprintf(cmd, sizeof(cmd), "git stash pop stash@{%d} 2>&1", idx);
      else
         snprintf(cmd, sizeof(cmd), "git stash pop 2>&1");
   }
   else if (strcmp(action, "apply") == 0)
   {
      cJSON *jindex = cJSON_GetObjectItemCaseSensitive(args, "index");
      int idx = (cJSON_IsNumber(jindex)) ? jindex->valueint : 0;
      snprintf(cmd, sizeof(cmd), "git stash apply stash@{%d} 2>&1", idx);
   }
   else if (strcmp(action, "list") == 0)
   {
      snprintf(cmd, sizeof(cmd), "git stash list 2>&1");
   }
   else if (strcmp(action, "drop") == 0)
   {
      cJSON *jindex = cJSON_GetObjectItemCaseSensitive(args, "index");
      int idx = (cJSON_IsNumber(jindex)) ? jindex->valueint : 0;
      snprintf(cmd, sizeof(cmd), "git stash drop stash@{%d} 2>&1", idx);
   }
   else
   {
      return mcp_text("error: unknown action. Use push/pop/apply/list/drop");
   }

   char *out = mcp_git_run(cmd, &rc);
   if (rc != 0)
   {
      cJSON *r = mcp_error("error: git stash %s failed: %s", out ? out : "unknown");
      free(out);
      return r;
   }

   cJSON *r = mcp_text(out && out[0] ? out : "(no output)");
   free(out);
   return r;
}

/* --- git_tag --- */

cJSON *handle_git_tag(cJSON *args)
{
   cJSON *jaction = cJSON_GetObjectItemCaseSensitive(args, "action");
   const char *action =
       (cJSON_IsString(jaction) && jaction->valuestring[0]) ? jaction->valuestring : "list";

   int rc;
   char cmd[1024];

   if (strcmp(action, "list") == 0)
   {
      snprintf(cmd, sizeof(cmd), "git tag --sort=-creatordate -n1 2>&1");

      char *out = mcp_git_run(cmd, &rc);
      if (rc != 0)
      {
         cJSON *r = mcp_error("error: git tag list failed: %s", out ? out : "unknown");
         free(out);
         return r;
      }
      cJSON *r = mcp_text(out && out[0] ? out : "(no tags)");
      free(out);
      return r;
   }

   cJSON *jname = cJSON_GetObjectItemCaseSensitive(args, "name");
   if (!cJSON_IsString(jname) || !jname->valuestring[0])
      return mcp_text("error: 'name' parameter is required for create/delete");

   char *esc_name = shell_escape(jname->valuestring);

   if (strcmp(action, "create") == 0)
   {
      cJSON *jmsg = cJSON_GetObjectItemCaseSensitive(args, "message");
      cJSON *jref = cJSON_GetObjectItemCaseSensitive(args, "ref");

      if (cJSON_IsString(jmsg) && jmsg->valuestring[0])
      {
         char *esc_msg = shell_escape(jmsg->valuestring);
         if (cJSON_IsString(jref) && jref->valuestring[0])
         {
            char *esc_ref = shell_escape(jref->valuestring);
            snprintf(cmd, sizeof(cmd), "git tag -a '%s' -m '%s' '%s' 2>&1", esc_name, esc_msg,
                     esc_ref);
            free(esc_ref);
         }
         else
         {
            snprintf(cmd, sizeof(cmd), "git tag -a '%s' -m '%s' 2>&1", esc_name, esc_msg);
         }
         free(esc_msg);
      }
      else
      {
         if (cJSON_IsString(jref) && jref->valuestring[0])
         {
            char *esc_ref = shell_escape(jref->valuestring);
            snprintf(cmd, sizeof(cmd), "git tag '%s' '%s' 2>&1", esc_name, esc_ref);
            free(esc_ref);
         }
         else
         {
            snprintf(cmd, sizeof(cmd), "git tag '%s' 2>&1", esc_name);
         }
      }
   }
   else if (strcmp(action, "delete") == 0)
   {
      snprintf(cmd, sizeof(cmd), "git tag -d '%s' 2>&1", esc_name);
   }
   else
   {
      free(esc_name);
      return mcp_text("error: unknown action. Use create/list/delete");
   }

   free(esc_name);

   char *out = mcp_git_run(cmd, &rc);
   if (rc != 0)
   {
      cJSON *r = mcp_error("error: git tag %s failed: %s", out ? out : "unknown");
      free(out);
      return r;
   }

   char result[512];
   if (strcmp(action, "create") == 0)
      snprintf(result, sizeof(result), "tagged: %s", jname->valuestring);
   else
      snprintf(result, sizeof(result), "deleted tag: %s", jname->valuestring);

   free(out);
   return mcp_text(result);
}

/* --- git_fetch --- */

typedef struct
{
   char *head_commit;
   char *head_ref;
   char *local_refs;
   char *index_entries;
   char *tracked_worktree;
   char *untracked_contents;
   char *status;
} fetch_state_t;

enum
{
   FETCH_STATE_OK = 0,
   FETCH_STATE_HEAD_UNRESOLVED = 1,
   FETCH_STATE_UNAVAILABLE = -1,
};

static void fetch_state_free(fetch_state_t *state)
{
   if (!state)
      return;
   free(state->head_commit);
   free(state->head_ref);
   free(state->local_refs);
   free(state->index_entries);
   free(state->tracked_worktree);
   free(state->untracked_contents);
   free(state->status);
   memset(state, 0, sizeof(*state));
}

/* A fetch is allowed to move remote-tracking refs and FETCH_HEAD only. Capture
 * every local checkout surface that must remain byte-for-byte unchanged:
 * symbolic HEAD + its commit, every local branch ref, index entries, tracked
 * worktree content, and untracked paths/content. Requiring a resolvable HEAD
 * also refuses an unborn checkout before a network operation can make its
 * recovery more confusing. */
static void fetch_state_trim_eol(char *value)
{
   if (!value)
      return;
   size_t len = strlen(value);
   while (len > 0 && (value[len - 1] == '\n' || value[len - 1] == '\r'))
      value[--len] = '\0';
}

/* On success, the caller owns the allocations and must call
 * fetch_state_free. On either failure the state is already empty. */
static int fetch_state_capture(fetch_state_t *state)
{
   if (!state)
      return -1;
   memset(state, 0, sizeof(*state));

   int rc = 0;
   state->head_commit = mcp_git_run("git rev-parse --verify 'HEAD^{commit}' 2>/dev/null", &rc);
   if (rc != 0 || !state->head_commit || !state->head_commit[0])
   {
      fetch_state_free(state);
      char *git_dir = mcp_git_run("git rev-parse --git-dir 2>/dev/null", &rc);
      int result = (rc == 0 && git_dir && git_dir[0]) ? FETCH_STATE_HEAD_UNRESOLVED
                                                      : FETCH_STATE_UNAVAILABLE;
      free(git_dir);
      return result;
   }

   /* symbolic-ref fails for a detached HEAD; in that case the literal marker
    * plus head_commit still distinguishes detached from attached state. */
   state->head_ref = mcp_git_run("git symbolic-ref -q HEAD 2>/dev/null || printf DETACHED", &rc);
   if (rc != 0 || !state->head_ref)
      goto fail;

   state->local_refs = mcp_git_run(
       "LC_ALL=C git for-each-ref --sort=refname --format='%(refname) %(objectname)' refs/heads "
       "2>/dev/null",
       &rc);
   if (rc != 0 || !state->local_refs)
      goto fail;

   state->index_entries = mcp_git_run(
       "git ls-files --stage -z 2>/dev/null | git hash-object --stdin 2>/dev/null", &rc);
   if (rc != 0 || !state->index_entries)
      goto fail;

   state->tracked_worktree = mcp_git_run(
       "git diff --no-ext-diff --binary 2>/dev/null | git hash-object --stdin 2>/dev/null", &rc);
   if (rc != 0 || !state->tracked_worktree)
      goto fail;

   /* status records the untracked path set; this additional digest detects a
    * content change at an unchanged untracked path. -z/xargs keeps arbitrary
    * filenames intact. Ignored files are outside Git's checkout state. */
   state->untracked_contents = mcp_git_run(
       "git ls-files --others --exclude-standard -z 2>/dev/null | "
       "xargs -0 -r git hash-object -- 2>/dev/null | git hash-object --stdin 2>/dev/null",
       &rc);
   if (rc != 0 || !state->untracked_contents)
      goto fail;

   state->status =
       mcp_git_run("LC_ALL=C git status --porcelain=v1 --untracked-files=all 2>/dev/null", &rc);
   if (rc != 0 || !state->status)
      goto fail;
   fetch_state_trim_eol(state->head_commit);
   fetch_state_trim_eol(state->head_ref);
   fetch_state_trim_eol(state->local_refs);
   fetch_state_trim_eol(state->index_entries);
   fetch_state_trim_eol(state->tracked_worktree);
   fetch_state_trim_eol(state->untracked_contents);
   fetch_state_trim_eol(state->status);
   return FETCH_STATE_OK;

fail:
   fetch_state_free(state);
   return FETCH_STATE_UNAVAILABLE;
}

static int fetch_state_equal(const fetch_state_t *a, const fetch_state_t *b)
{
   return a && b && a->head_commit && b->head_commit && a->head_ref && b->head_ref &&
          a->local_refs && b->local_refs && a->index_entries && b->index_entries &&
          a->tracked_worktree && b->tracked_worktree && a->untracked_contents &&
          b->untracked_contents && a->status && b->status &&
          strcmp(a->head_commit, b->head_commit) == 0 && strcmp(a->head_ref, b->head_ref) == 0 &&
          strcmp(a->local_refs, b->local_refs) == 0 &&
          strcmp(a->index_entries, b->index_entries) == 0 &&
          strcmp(a->tracked_worktree, b->tracked_worktree) == 0 &&
          strcmp(a->untracked_contents, b->untracked_contents) == 0 &&
          strcmp(a->status, b->status) == 0;
}

/* The remote name is also embedded below refs/remotes/<name>. Keep that ref
 * namespace unambiguous and reject option-looking repository arguments. */
static int fetch_remote_name_safe(const char *remote)
{
   if (!remote || !remote[0] || strlen(remote) > 200 || remote[0] == '-' || remote[0] == '.' ||
       strcmp(remote, "HEAD") == 0 || strcmp(remote, "FETCH_HEAD") == 0 ||
       strcmp(remote, "ORIG_HEAD") == 0 || strstr(remote, "..") || strstr(remote, "@{") ||
       strstr(remote, "//"))
      return 0;
   size_t len = strlen(remote);
   if (remote[len - 1] == '/' || remote[len - 1] == '.' || strstr(remote, ".lock"))
      return 0;
   for (const unsigned char *p = (const unsigned char *)remote; *p; p++)
      if (!isalnum(*p) && *p != '-' && *p != '_' && *p != '.' && *p != '/' && *p != '+')
         return 0;
   return 1;
}

cJSON *handle_git_fetch(cJSON *args)
{
   cJSON *jprune = cJSON_GetObjectItemCaseSensitive(args, "prune");
   int prune = (jprune && cJSON_IsTrue(jprune)) ? 1 : 0;

   cJSON *jremote = cJSON_GetObjectItemCaseSensitive(args, "remote");
   const char *remote =
       (cJSON_IsString(jremote) && jremote->valuestring[0]) ? jremote->valuestring : "origin";

   if (!fetch_remote_name_safe(remote))
      return mcp_error("error: %s", "fetch remote name is invalid");

   fetch_state_t before;
   int before_rc = fetch_state_capture(&before);
   if (before_rc == FETCH_STATE_HEAD_UNRESOLVED)
      return mcp_error("error: %s", "fetch refused because HEAD does not resolve");
   if (before_rc != FETCH_STATE_OK)
      return mcp_error("error: %s", "fetch refused because checkout state cannot be captured");

   char *esc_remote = shell_escape(remote);
   char refspec[512];
   snprintf(refspec, sizeof(refspec), "+refs/heads/*:refs/remotes/%s/*", remote);
   char *esc_refspec = shell_escape(refspec);
   char cmd[1024];
   /* A positional refspec is not enough to contain --prune: Git still consults
    * remote.<name>.fetch for its prune map. An explicitly empty --refmap
    * suppresses that configured mapping; the one positional refspec below is
    * then the only fetch and prune destination. */
   if (prune)
      snprintf(cmd, sizeof(cmd),
               "git fetch --no-tags --no-prune-tags --prune --refmap= '%s' '%s' 2>&1", esc_remote,
               esc_refspec);
   else
      snprintf(cmd, sizeof(cmd), "git fetch --no-tags --no-prune-tags --refmap= '%s' '%s' 2>&1",
               esc_remote, esc_refspec);
   free(esc_remote);
   free(esc_refspec);

   int rc;
   char *out = mcp_git_run(cmd, &rc);

   fetch_state_t after;
   int after_ok = fetch_state_capture(&after) == FETCH_STATE_OK;
   int unchanged = after_ok && fetch_state_equal(&before, &after);
   fetch_state_free(&after);
   if (!unchanged)
   {
      cJSON *r = mcp_error(
          "error: unsafe fetch detected: HEAD, a local branch, the index, or the worktree "
          "changed (pre-fetch HEAD %s). Stop and inspect the checkout before continuing.",
          before.head_commit ? before.head_commit : "unknown");
      fetch_state_free(&before);
      free(out);
      return r;
   }
   fetch_state_free(&before);
   if (rc != 0)
   {
      cJSON *r = mcp_error("error: git fetch failed: %s", out ? out : "unknown");
      free(out);
      return r;
   }

   char result[512];
   snprintf(result, sizeof(result),
            "fetched branches into refs/remotes/%s/*%s; HEAD and local checkout unchanged", remote,
            prune ? " (pruned remote-tracking refs)" : "");
   free(out);
   return mcp_text(result);
}
