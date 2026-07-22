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
      return mcp_text("error: 'action' parameter is required (create/switch/list/delete/claim)");

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
      return mcp_text("error: 'name' parameter is required for create/switch/delete");

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

      char cmd[512];
      snprintf(cmd, sizeof(cmd), "git checkout '%s' 2>&1", esc_name);
      int rc;
      char *out = mcp_git_run(cmd, &rc);
      if (rc != 0)
      {
         cJSON *r = mcp_error("error: git switch failed: %s", out ? out : "unknown");
         free(out);
         free(esc_name);
         return r;
      }
      free(out);

      char result[256];
      snprintf(result, sizeof(result), "switched to %s", jname->valuestring);
      free(esc_name);
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
      if (!branch_own_check(jname->valuestring, owner, sizeof(owner)))
      {
         char err[512];
         snprintf(err, sizeof(err),
                  "error: branch '%s' is owned by session %.20s. "
                  "Cannot claim a branch owned by another session.",
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

   free(esc_name);
   return mcp_text("error: unknown action. Use create/switch/list/delete/claim");
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

cJSON *handle_git_fetch(cJSON *args)
{
   cJSON *jprune = cJSON_GetObjectItemCaseSensitive(args, "prune");
   int prune = (jprune && cJSON_IsTrue(jprune)) ? 1 : 0;

   cJSON *jremote = cJSON_GetObjectItemCaseSensitive(args, "remote");
   const char *remote =
       (cJSON_IsString(jremote) && jremote->valuestring[0]) ? jremote->valuestring : "origin";

   char *esc_remote = shell_escape(remote);
   char cmd[512];
   if (prune)
      snprintf(cmd, sizeof(cmd), "git fetch --prune '%s' 2>&1", esc_remote);
   else
      snprintf(cmd, sizeof(cmd), "git fetch '%s' 2>&1", esc_remote);
   free(esc_remote);

   int rc;
   char *out = mcp_git_run(cmd, &rc);
   if (rc != 0)
   {
      cJSON *r = mcp_error("error: git fetch failed: %s", out ? out : "unknown");
      free(out);
      return r;
   }

   char result[512];
   snprintf(result, sizeof(result), "fetched from %s%s", remote, prune ? " (pruned)" : "");
   free(out);
   return mcp_text(result);
}
