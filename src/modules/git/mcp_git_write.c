/* mcp_git_write.c: MCP git write handlers (commit, push, pull, clone, reset, restore) */
#include "aimee.h"
#include "cJSON.h"
#include "config.h"
#include "guardrails.h"
#include "git_verify.h"
#include "mcp_git.h"
#include "util.h"
#include "branch_ownership.h"
#include "agent_config.h" /* agent_get_request_vault_principal */
#include "git_forge_vault.h"
#include <dirent.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>

#define GIT_BUF_SIZE 65536
#define SUMMARY_MAX  4096

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

static cJSON *mcp_error(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static cJSON *mcp_error(const char *fmt, ...)
{
   char buf[1024];
   va_list ap;
   va_start(ap, fmt);
   vsnprintf(buf, sizeof(buf), fmt, ap);
   va_end(ap);
   return mcp_text(buf);
}

/* Read a git config value through mcp_git_run — the SAME runner the commit
 * below uses. These tools do not execute in the server process's own working
 * directory (a workspace provider decides where git runs, and a detached
 * workspace runs it on the client entirely), so resolving config in-process
 * would consult a directory that is not the checkout and silently find
 * nothing. */
static int mcp_git_config_reader(const char *key, char *out, size_t out_len, void *ud)
{
   (void)ud;
   if (!out || !out_len)
      return 0;
   out[0] = '\0';

   char cmd[384];
   snprintf(cmd, sizeof(cmd), "git config --local --get %s 2>/dev/null", key);
   int rc = 0;
   char *got = mcp_git_run(cmd, &rc);
   if (rc == 0)
   {
      snprintf(out, out_len, "%s", got ? got : "");
      out[strcspn(out, "\r\n")] = '\0';
   }
   free(got);
   if (out[0])
      return 1;

   /* Delegate launchers deliberately mask ambient Git config so untrusted raw
    * git commands cannot inherit credential helpers, hooks, or transport
    * settings. Identity is different: read only these caller-selected
    * user.name/user.email keys from the operator's normal global config on the
    * same workspace runner, then pass the values back as explicit `git -c`
    * arguments. Unmasking this read cannot alter commit behaviour. */
   snprintf(cmd, sizeof(cmd),
            "unset GIT_CONFIG_NOSYSTEM GIT_CONFIG_SYSTEM GIT_CONFIG_GLOBAL; "
            "git config --global --get %s 2>/dev/null",
            key);
   got = mcp_git_run(cmd, &rc);
   if (rc == 0)
   {
      snprintf(out, out_len, "%s", got ? got : "");
      out[strcspn(out, "\r\n")] = '\0';
   }
   free(got);
   return out[0] ? 1 : 0;
}

/* See mcp_git.h. Commit AS THE CONFIGURED OPERATOR, or not at all.
 *
 * git_ops points GIT_CONFIG_GLOBAL/SYSTEM at /dev/null, so git has no ambient
 * identity to fall back on: aimee supplies one or the commit has no author.
 * Refuse rather than invent a persona. A bot author is not free either, since
 * GitHub adds a Co-authored-by trailer to the squash of any PR whose commits
 * carry two distinct authors, and the standing directive forbids those.
 *
 * Shared because every path that CREATES a commit needs the same identity:
 * git_commit, and the merge/rebase/cherry-pick/revert continuations. */
int mcp_git_identity_flags(char *out, size_t out_len)
{
   if (!out || !out_len)
      return -1;
   out[0] = '\0';
   char name[256] = "", email[256] = "";
   int have = git_identity_resolve_with(agent_get_request_vault_principal(), mcp_git_config_reader,
                                        NULL, name, sizeof(name), email, sizeof(email));
   if (have <= 0)
      return have;
   char *esc_name = shell_quote(name);
   char *esc_email = shell_quote(email);
   snprintf(out, out_len, "-c user.name=%s -c user.email=%s", esc_name, esc_email);
   free(esc_name);
   free(esc_email);
   return 1;
}

/* See mcp_git.h. */
const char *mcp_git_identity_error(int rc)
{
   if (rc < 0)
      return "error: could not read the git identity from the vault";
   return "error: no git identity is configured, so this commit would have no author. "
          "Set one on this checkout with `git config user.name` and `git config user.email` "
          "(both, or neither takes effect), or seal AIMEE_GIT_AUTHOR_NAME and "
          "AIMEE_GIT_AUTHOR_EMAIL into the vault to apply one everywhere.";
}

/* Return a standard error response for main branch protection.
 * Caller should return the result directly from the handler. */
static cJSON *main_branch_blocked(const char *operation)
{
   char buf[512];
   snprintf(buf, sizeof(buf),
            "error: %s blocked — writing to the main branch is not allowed. "
            "Create a feature branch first.",
            operation);
   return mcp_text(buf);
}
/* --- git_commit --- */

cJSON *handle_git_commit(cJSON *args)
{
   /* Before anything else: a commit into a mirror's server-side reconstruction is
    * discarded on the client's next change, and used to be reported as a success with
    * a SHA no reachable checkout contained. */
   {
      cJSON *blocked = mcp_git_durability_guard("commit");
      if (blocked)
         return blocked;
   }

   cJSON *jmsg = cJSON_GetObjectItemCaseSensitive(args, "message");
   if (!cJSON_IsString(jmsg) || !jmsg->valuestring[0])
      return mcp_text("error: 'message' parameter is required");

   /* Standing directive: no AI co-authorship trailers / "Generated with"
    * attribution in commits. Strip in place (shrink-only, so the cJSON-owned
    * buffer is safe) rather than reject, so agent commits stay unattended. */
   strip_ai_attribution(jmsg->valuestring);
   if (!jmsg->valuestring[0])
      return mcp_text("error: commit message was only AI attribution lines; "
                      "provide a real message (no Co-Authored-By / 'Generated with' trailers)");

   /* Fetch branch once — used for main-branch guard and ownership check */
   char branch[256] = "";
   get_current_branch(branch, sizeof(branch));
   if (strcmp(branch, "main") == 0 || strcmp(branch, "master") == 0)
      return main_branch_blocked("commit");
   {
      cJSON *blocked = branch_own_guard_for(branch, "commit");
      if (blocked)
         return blocked;
   }

   cJSON *jfiles = cJSON_GetObjectItemCaseSensitive(args, "files");

   /* Stage files */
   if (jfiles && cJSON_IsArray(jfiles) && cJSON_GetArraySize(jfiles) > 0)
   {
      /* Stage specific files, skipping sensitive ones */
      char add_cmd[8192] = "git add --";
      size_t cmd_len = strlen(add_cmd);
      int skipped = 0;
      int count = cJSON_GetArraySize(jfiles);

      for (int i = 0; i < count; i++)
      {
         cJSON *f = cJSON_GetArrayItem(jfiles, i);
         if (!cJSON_IsString(f))
            continue;
         if (is_sensitive_file(f->valuestring))
         {
            skipped++;
            continue;
         }
         char *esc = shell_quote(f->valuestring);
         int n = snprintf(add_cmd + cmd_len, sizeof(add_cmd) - cmd_len, " %s", esc);
         free(esc);
         if (n < 0 || cmd_len + (size_t)n >= sizeof(add_cmd))
         {
            cmd_len = sizeof(add_cmd) - 1;
            break;
         }
         cmd_len += (size_t)n;
      }

      if (cmd_len > strlen("git add --") && cmd_len + 6 < sizeof(add_cmd))
      {
         memcpy(add_cmd + cmd_len, " 2>&1", 6);
         cmd_len += 5;
         int rc;
         char *out = mcp_git_run(add_cmd, &rc);
         if (rc != 0)
         {
            cJSON *r = mcp_error("error: git add failed: %s", out ? out : "unknown");
            free(out);
            return r;
         }
         free(out);
      }

      if (skipped)
      {
         char warn[256];
         snprintf(warn, sizeof(warn),
                  "warning: skipped %d sensitive file%s (.env, credentials, keys)", skipped,
                  skipped == 1 ? "" : "s");
         /* Continue with commit, but prepend warning */
      }
   }
   else
   {
      /* Stage all modified tracked files (not untracked) */
      int rc;
      char *out = mcp_git_run("git add -u 2>&1", &rc);
      if (rc != 0)
      {
         cJSON *r = mcp_error("error: git add -u failed: %s", out ? out : "unknown");
         free(out);
         return r;
      }
      free(out);
   }

   char ident[768];
   int have_identity = mcp_git_identity_flags(ident, sizeof(ident));
   if (have_identity <= 0)
      return mcp_text(mcp_git_identity_error(have_identity));

   char *esc_msg = shell_quote(jmsg->valuestring);
   char commit_cmd[8192];
   snprintf(commit_cmd, sizeof(commit_cmd), "git %s commit -m %s 2>&1", ident, esc_msg);
   free(esc_msg);

   int rc;
   char *out = mcp_git_run(commit_cmd, &rc);
   if (rc != 0)
   {
      cJSON *r = mcp_error("error: git commit failed: %s", out ? out : "unknown");
      free(out);
      return r;
   }
   free(out);

   /* Get commit hash and diffstat */
   char *hash_out = mcp_git_run("git log -1 --format='%h' 2>&1", &rc);
   char *stat_out = mcp_git_run("git diff --stat HEAD~1 HEAD 2>&1", &rc);

   char result[SUMMARY_MAX];
   int pos = 0;

   /* Extract short hash */
   char hash[16] = "unknown";
   if (hash_out)
   {
      char *nl = strchr(hash_out, '\n');
      if (nl)
         *nl = '\0';
      snprintf(hash, sizeof(hash), "%s", hash_out);
      free(hash_out);
   }

   pos = str_appendf(result, pos, (int)sizeof(result), "committed: %s \"%s\"", hash,
                     jmsg->valuestring);

   /* Extract just the summary line from diffstat */
   if (stat_out)
   {
      /* Last non-empty line is the summary */
      char *last = NULL;
      char *l = stat_out;
      while (l && *l)
      {
         char *nl = strchr(l, '\n');
         if (nl)
            *nl = '\0';
         if (*l)
            last = l;
         l = nl ? nl + 1 : NULL;
      }
      if (last)
         pos = str_appendf(result, pos, (int)sizeof(result), "\n%s", last);
      free(stat_out);
   }

   (void)pos;
   return mcp_text(result);
}

/* --- git_push --- */

cJSON *handle_git_push(cJSON *args)
{
   /* Fetch branch once — reused for main guard, ownership, and merged-PR checks */
   char branch[256] = "";
   get_current_branch(branch, sizeof(branch));
   if (strcmp(branch, "main") == 0 || strcmp(branch, "master") == 0)
      return main_branch_blocked("push");
   {
      cJSON *blocked = branch_own_guard_for(branch, "push");
      if (blocked)
         return blocked;
   }

   /* Merged-PR enforcement: block pushes to branches with merged PRs */
   if (check_branch_has_merged_pr_for(branch))
      return mcp_text("error: branch has a merged PR. Do not push to merged branches. "
                      "Create a new branch for new work.");

   /* Verify gate. verify_gate_blocks honors scope (current project only unless
    * cross-project verify is enabled) and the global verify master switch, and
    * never auto-generates config for an out-of-scope/unconfigured repo. */
   {
      char verify_msg[256];
      if (verify_gate_blocks(run_cmd_get_cwd(), NULL, verify_msg, sizeof(verify_msg)))
      {
         char buf[512];
         snprintf(buf, sizeof(buf), "error: push blocked: %s", verify_msg);
         return mcp_text(buf);
      }
   }

   cJSON *jforce = cJSON_GetObjectItemCaseSensitive(args, "force");
   int force = (jforce && cJSON_IsTrue(jforce)) ? 1 : 0;

   /* Mirror push: replaces all remote refs with local refs */
   cJSON *jmirror = cJSON_GetObjectItemCaseSensitive(args, "mirror");
   if (jmirror && cJSON_IsTrue(jmirror))
   {
      int rc;
      char *out = mcp_git_run("git push origin --mirror 2>&1", &rc);
      if (rc != 0)
      {
         cJSON *r = mcp_error("error: git push --mirror failed: %s", out ? out : "unknown");
         free(out);
         return r;
      }
      char result[GIT_BUF_SIZE];
      snprintf(result, sizeof(result),
               "mirror push complete — remote now matches local refs exactly.\n%s", out ? out : "");
      free(out);
      return mcp_text(result);
   }

   /* Determine which branch to push.
    * In a worktree, HEAD is the session branch (aimee/session/<id>), NOT the user's
    * working branch. Look up the session's owned branch from branch_ownership instead.
    * In the normal path, branch is already set from the guard check above. */
   int rc;

   if (mcp_git_get_worktree())
   {
      /* Save the current HEAD branch before the ownership lookup overwrites it. */
      char head_branch[256];
      snprintf(head_branch, sizeof(head_branch), "%s", branch);

      if (branch_own_get_session_branch(branch, sizeof(branch)) != 0)
      {
         /* No registered branch found for this session.  If HEAD is already a
          * real user branch (not the aimee/session/... sentinel), use it directly —
          * the user may have created the branch via git directly rather than through
          * aimee's git_branch tool. */
         if (strncmp(head_branch, "aimee/session/", 14) == 0)
            return mcp_text("error: in worktree mode but no owned branch found. "
                            "Use git_branch action=create to create and register a branch first.");
         snprintf(branch, sizeof(branch), "%s", head_branch);
      }
   }
   else if (!branch[0])
   {
      /* Fallback: branch fetch failed earlier; try again now */
      char *branch_out = mcp_git_run("git rev-parse --abbrev-ref HEAD 2>&1", &rc);
      if (rc != 0 || !branch_out)
      {
         cJSON *r = mcp_text("error: not on a branch");
         free(branch_out);
         return r;
      }
      char *nl = strchr(branch_out, '\n');
      if (nl)
         *nl = '\0';
      snprintf(branch, sizeof(branch), "%s", branch_out);
      free(branch_out);
   }

   /* Build push command.
    * In worktree mode, always push by explicit refspec since HEAD != the target branch. */
   char cmd[512];
   if (mcp_git_get_worktree())
   {
      /* Push the owned branch to origin with explicit refspec */
      if (force)
         snprintf(cmd, sizeof(cmd), "git push --force-with-lease -u origin '%s' 2>&1", branch);
      else
         snprintf(cmd, sizeof(cmd), "git push -u origin '%s' 2>&1", branch);
   }
   else
   {
      /* Check if upstream exists and whether it matches the local branch name */
      char *upstream = mcp_git_run("git rev-parse --abbrev-ref @{upstream} 2>&1", &rc);
      int has_upstream = (rc == 0);
      int upstream_matches = 0;
      if (has_upstream && upstream)
      {
         char *slash = strchr(upstream, '/');
         const char *upstream_branch = slash ? slash + 1 : upstream;
         char *up_nl = strchr(upstream_branch, '\n');
         if (up_nl)
            *up_nl = '\0';
         upstream_matches = (strcmp(upstream_branch, branch) == 0);
      }
      free(upstream);

      if (has_upstream && upstream_matches)
      {
         if (force)
            snprintf(cmd, sizeof(cmd), "git push --force-with-lease 2>&1");
         else
            snprintf(cmd, sizeof(cmd), "git push 2>&1");
      }
      else
      {
         snprintf(cmd, sizeof(cmd), "git push -u origin '%s' 2>&1", branch);
      }
   }

   char *out = mcp_git_run(cmd, &rc);
   if (rc != 0)
   {
      cJSON *r = mcp_error("error: git push failed: %s", out ? out : "unknown");
      free(out);
      return r;
   }
   free(out);

   /* Get the commit hash we pushed */
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

   char result[512];
   snprintf(result, sizeof(result), "pushed: %s -> origin/%s (%s)%s", branch, branch, hash,
            force ? " (force-with-lease)" : "");
   return mcp_text(result);
}

/* --- git_pull --- */

cJSON *handle_git_pull(cJSON *args)
{
   {
      cJSON *blocked = mcp_git_durability_guard("pull");
      if (blocked)
         return blocked;
   }
   cJSON *jrebase = cJSON_GetObjectItemCaseSensitive(args, "rebase");
   int rebase = (jrebase && cJSON_IsTrue(jrebase)) ? 1 : 0;

   int rc;
   char *branch_out = mcp_git_run("git rev-parse --abbrev-ref HEAD 2>&1", &rc);
   char branch[256] = "";
   if (branch_out)
   {
      char *nl = strchr(branch_out, '\n');
      if (nl)
         *nl = '\0';
      snprintf(branch, sizeof(branch), "%s", branch_out);
      free(branch_out);
   }

   /* Get pre-pull HEAD for summary */
   char *pre_hash = mcp_git_run("git rev-parse --short HEAD 2>/dev/null", &rc);
   char pre[16] = "";
   if (pre_hash)
   {
      char *nl = strchr(pre_hash, '\n');
      if (nl)
         *nl = '\0';
      snprintf(pre, sizeof(pre), "%s", pre_hash);
      free(pre_hash);
   }

   char cmd[512];
   if (rebase)
      snprintf(cmd, sizeof(cmd), "git pull --rebase 2>&1");
   else
      snprintf(cmd, sizeof(cmd), "git pull 2>&1");

   char *out = mcp_git_run(cmd, &rc);
   if (rc != 0)
   {
      cJSON *r = mcp_error("error: git pull failed: %s", out ? out : "unknown");
      free(out);
      return r;
   }

   /* Get post-pull HEAD */
   char *post_hash = mcp_git_run("git rev-parse --short HEAD 2>/dev/null", &rc);
   char post[16] = "";
   if (post_hash)
   {
      char *nl = strchr(post_hash, '\n');
      if (nl)
         *nl = '\0';
      snprintf(post, sizeof(post), "%s", post_hash);
      free(post_hash);
   }

   char result[512];
   if (strcmp(pre, post) == 0)
      snprintf(result, sizeof(result), "already up to date on %s (%s)", branch, post);
   else
      snprintf(result, sizeof(result), "pulled: %s %s..%s%s", branch, pre, post,
               rebase ? " (rebased)" : "");

   free(out);
   return mcp_text(result);
}

/* --- git_clone --- */

cJSON *handle_git_clone(cJSON *args)
{
   cJSON *jurl = cJSON_GetObjectItemCaseSensitive(args, "url");
   if (!cJSON_IsString(jurl) || !jurl->valuestring[0])
      return mcp_text("error: 'url' parameter is required");

   cJSON *jpath = cJSON_GetObjectItemCaseSensitive(args, "path");
   cJSON *jbranch = cJSON_GetObjectItemCaseSensitive(args, "branch");
   cJSON *jdepth = cJSON_GetObjectItemCaseSensitive(args, "depth");

   char *esc_url = shell_quote(jurl->valuestring);

   char cmd[2048];
   int pos = snprintf(cmd, sizeof(cmd), "git clone");
   if (pos < 0 || (size_t)pos >= sizeof(cmd))
      pos = (int)(sizeof(cmd) - 1);

   if (cJSON_IsString(jbranch) && jbranch->valuestring[0])
   {
      char *esc = shell_quote(jbranch->valuestring);
      int n = snprintf(cmd + pos, sizeof(cmd) - (size_t)pos, " -b %s", esc);
      free(esc);
      if (n > 0 && (size_t)pos + (size_t)n < sizeof(cmd))
         pos += n;
   }
   if (cJSON_IsNumber(jdepth) && jdepth->valueint > 0)
   {
      int n = snprintf(cmd + pos, sizeof(cmd) - (size_t)pos, " --depth %d", jdepth->valueint);
      if (n > 0 && (size_t)pos + (size_t)n < sizeof(cmd))
         pos += n;
   }

   {
      int n = snprintf(cmd + pos, sizeof(cmd) - (size_t)pos, " %s", esc_url);
      if (n > 0 && (size_t)pos + (size_t)n < sizeof(cmd))
         pos += n;
   }

   if (cJSON_IsString(jpath) && jpath->valuestring[0])
   {
      char *esc = shell_quote(jpath->valuestring);
      int n = snprintf(cmd + pos, sizeof(cmd) - (size_t)pos, " %s", esc);
      free(esc);
      if (n > 0 && (size_t)pos + (size_t)n < sizeof(cmd))
         pos += n;
   }

   if ((size_t)pos + 6 < sizeof(cmd))
      memcpy(cmd + pos, " 2>&1", 6);
   free(esc_url);

   int rc;
   char *out = mcp_git_run(cmd, &rc);
   if (rc != 0)
   {
      cJSON *r = mcp_error("error: git clone failed: %s", out ? out : "unknown");
      free(out);
      return r;
   }

   char result[512];
   snprintf(result, sizeof(result), "cloned: %s", jurl->valuestring);
   free(out);
   return mcp_text(result);
}

/* --- git_reset --- */

cJSON *handle_git_reset(cJSON *args)
{
   {
      cJSON *blocked = mcp_git_durability_guard("reset");
      if (blocked)
         return blocked;
   }
   /* Fetch branch once — used for main guard and ownership check */
   char branch[256] = "";
   get_current_branch(branch, sizeof(branch));
   if (strcmp(branch, "main") == 0 || strcmp(branch, "master") == 0)
      return main_branch_blocked("reset");
   {
      cJSON *blocked = branch_own_guard_for(branch, "reset");
      if (blocked)
         return blocked;
   }

   cJSON *jref = cJSON_GetObjectItemCaseSensitive(args, "ref");
   const char *ref = (cJSON_IsString(jref) && jref->valuestring[0]) ? jref->valuestring : "HEAD~1";

   cJSON *jmode = cJSON_GetObjectItemCaseSensitive(args, "mode");
   const char *mode =
       (cJSON_IsString(jmode) && jmode->valuestring[0]) ? jmode->valuestring : "mixed";

   /* Validate mode */
   if (strcmp(mode, "soft") != 0 && strcmp(mode, "mixed") != 0 && strcmp(mode, "hard") != 0)
      return mcp_text("error: mode must be soft, mixed, or hard");

   char *esc_ref = shell_quote(ref);
   char cmd[512];
   snprintf(cmd, sizeof(cmd), "git reset --%s %s 2>&1", mode, esc_ref);
   free(esc_ref);

   int rc;
   char *out = mcp_git_run(cmd, &rc);
   if (rc != 0)
   {
      cJSON *r = mcp_error("error: git reset failed: %s", out ? out : "unknown");
      free(out);
      return r;
   }

   /* Get new HEAD */
   char *hash_out = mcp_git_run("git rev-parse --short HEAD 2>/dev/null", &rc);
   char hash[16] = "";
   if (hash_out)
   {
      char *nl = strchr(hash_out, '\n');
      if (nl)
         *nl = '\0';
      snprintf(hash, sizeof(hash), "%s", hash_out);
      free(hash_out);
   }

   char result[512];
   snprintf(result, sizeof(result), "reset (--%s) to %s, HEAD now at %s", mode, ref, hash);
   free(out);
   return mcp_text(result);
}

/* --- git_add ---
 *
 * Staging as its own operation. git_commit stages what it is about to commit and
 * nothing else (tracked files with -u, or the paths it was handed), so there was
 * no way to stage a NEW file, or to stage now and commit later. Sensitive paths
 * are dropped here exactly as git_commit drops them, and `all` is screened the
 * same way — the screen runs against the resulting index, not against the
 * caller's argument list, so `all` cannot sweep in a .env that a path list
 * would have been refused. */
cJSON *handle_git_add(cJSON *args)
{
   {
      cJSON *blocked = mcp_git_durability_guard("add");
      if (blocked)
         return blocked;
   }
   char branch[256] = "";
   get_current_branch(branch, sizeof(branch));
   if (strcmp(branch, "main") == 0 || strcmp(branch, "master") == 0)
      return main_branch_blocked("add");
   {
      cJSON *blocked = branch_own_guard_for(branch, "add");
      if (blocked)
         return blocked;
   }

   cJSON *jfiles = cJSON_GetObjectItemCaseSensitive(args, "files");
   cJSON *jall = cJSON_GetObjectItemCaseSensitive(args, "all");
   int all = (jall && cJSON_IsTrue(jall)) ? 1 : 0;
   int have_files = cJSON_IsArray(jfiles) && cJSON_GetArraySize(jfiles) > 0;

   if (!all && !have_files)
      return mcp_text("error: 'files' (array of paths) or 'all' (true, stages every change "
                      "including new files) is required");

   int skipped = 0;
   char cmd[8192];
   int pos;
   if (all)
   {
      pos = snprintf(cmd, sizeof(cmd), "git add -A");
   }
   else
   {
      pos = snprintf(cmd, sizeof(cmd), "git add --");
      int count = cJSON_GetArraySize(jfiles);
      for (int i = 0; i < count; i++)
      {
         cJSON *f = cJSON_GetArrayItem(jfiles, i);
         if (!cJSON_IsString(f))
            continue;
         if (is_sensitive_file(f->valuestring))
         {
            skipped++;
            continue;
         }
         char *esc = shell_quote(f->valuestring);
         pos = str_appendf(cmd, pos, (int)sizeof(cmd), " %s", esc);
         free(esc);
      }
      if (pos <= (int)strlen("git add --"))
         return mcp_text("error: every path given is a sensitive file (.env, credentials, keys); "
                         "nothing was staged");
   }
   str_appendf(cmd, pos, (int)sizeof(cmd), " 2>&1");

   int rc;
   char *out = mcp_git_run(cmd, &rc);
   if (rc != 0)
   {
      cJSON *r = mcp_error("error: git add failed: %s", out ? out : "unknown");
      free(out);
      return r;
   }
   free(out);

   /* Screen the INDEX, not the argument list: `all` stages by pattern, so this is
    * the only place that can see what it actually picked up. */
   char unstaged[2048] = "";
   int unstaged_n = mcp_git_unstage_sensitive(unstaged, sizeof(unstaged));

   char *staged = mcp_git_run("git diff --cached --name-only 2>/dev/null", &rc);
   int staged_n = 0;
   if (staged)
      for (const char *p = staged; *p; p++)
         if (*p == '\n')
            staged_n++;

   char result[SUMMARY_MAX];
   int rpos = snprintf(result, sizeof(result), "staged %d file(s)", staged_n);
   if (skipped)
      rpos =
          str_appendf(result, rpos, (int)sizeof(result),
                      "\nwarning: skipped %d sensitive path(s) (.env, credentials, keys)", skipped);
   if (unstaged_n)
      rpos = str_appendf(result, rpos, (int)sizeof(result),
                         "\nwarning: unstaged %d sensitive "
                         "file(s) that `all` had picked up: %s",
                         unstaged_n, unstaged);
   if (staged && staged[0])
      rpos = str_appendf(result, rpos, (int)sizeof(result), "\n%s", staged);
   (void)rpos;
   free(staged);
   return mcp_text(result);
}

/* See mcp_git.h. Kept next to git_add because that is what needs it: `add -A`
 * stages by pattern, and git_commit only screens paths it was handed. */
int mcp_git_unstage_sensitive(char *report, size_t report_len)
{
   if (report && report_len)
      report[0] = '\0';
   int rc = 0;
   char *staged = mcp_git_run("git diff --cached --name-only 2>/dev/null", &rc);
   if (!staged)
      return 0;

   int n = 0, rpos = 0;
   char *line = staged;
   while (line && *line)
   {
      char *nl = strchr(line, '\n');
      if (nl)
         *nl = '\0';
      if (*line && is_sensitive_file(line))
      {
         char *esc = shell_quote(line);
         char cmd[MAX_PATH_LEN + 64];
         snprintf(cmd, sizeof(cmd), "git restore --staged -- %s 2>&1", esc);
         free(esc);
         int unstage_rc = 0;
         free(mcp_git_run(cmd, &unstage_rc));
         if (unstage_rc == 0)
         {
            n++;
            if (report && report_len)
               rpos = str_appendf(report, rpos, (int)report_len, "%s%s", n > 1 ? ", " : "", line);
         }
      }
      line = nl ? nl + 1 : NULL;
   }
   free(staged);
   return n;
}

/* --- git_switch / git_checkout ---
 *
 * Pure routing to the handler that already owns each behaviour, so the model does
 * not have to know that "switch branches" lives under git_branch and "throw away
 * my edits to this file" lives under git_restore. No second implementation: the
 * worktree lock, the ownership registration and the restore semantics stay in
 * one place each. */
cJSON *handle_git_switch(cJSON *args)
{
   cJSON *jname = cJSON_GetObjectItemCaseSensitive(args, "name");
   cJSON *jref = cJSON_GetObjectItemCaseSensitive(args, "ref");
   if ((!cJSON_IsString(jname) || !jname->valuestring[0]) && cJSON_IsString(jref) &&
       jref->valuestring[0])
      cJSON_AddStringToObject(args, "name", jref->valuestring);
   if (!cJSON_IsString(cJSON_GetObjectItemCaseSensitive(args, "name")))
      return mcp_text("error: 'ref' (the branch to switch to) is required");

   cJSON *jaction = cJSON_GetObjectItemCaseSensitive(args, "action");
   if (jaction)
      cJSON_ReplaceItemInObject(args, "action", cJSON_CreateString("switch"));
   else
      cJSON_AddStringToObject(args, "action", "switch");
   return handle_git_branch(args);
}

cJSON *handle_git_checkout(cJSON *args)
{
   cJSON *jfiles = cJSON_GetObjectItemCaseSensitive(args, "files");
   if (cJSON_IsArray(jfiles) && cJSON_GetArraySize(jfiles) > 0)
   {
      /* Path-scoped checkout is a restore: `source` is what git spells as the ref
       * to take the content from. */
      cJSON *jref = cJSON_GetObjectItemCaseSensitive(args, "ref");
      if (cJSON_IsString(jref) && jref->valuestring[0] &&
          !cJSON_GetObjectItemCaseSensitive(args, "source"))
         cJSON_AddStringToObject(args, "source", jref->valuestring);
      return handle_git_restore(args);
   }
   return handle_git_switch(args);
}

/* --- git_restore --- */

cJSON *handle_git_restore(cJSON *args)
{
   /* Also covers checkout with a file list, which routes here. */
   {
      cJSON *blocked = mcp_git_durability_guard("restore");
      if (blocked)
         return blocked;
   }
   cJSON *jfiles = cJSON_GetObjectItemCaseSensitive(args, "files");
   cJSON *jstaged = cJSON_GetObjectItemCaseSensitive(args, "staged");
   cJSON *jsource = cJSON_GetObjectItemCaseSensitive(args, "source");
   int staged = (jstaged && cJSON_IsTrue(jstaged)) ? 1 : 0;

   if (!cJSON_IsArray(jfiles) || cJSON_GetArraySize(jfiles) == 0)
      return mcp_text("error: 'files' parameter is required (array of file paths)");

   /* Build file list */
   char file_args[4096] = "";
   int fpos = 0;
   int file_count = cJSON_GetArraySize(jfiles);
   for (int i = 0; i < file_count && i < 50; i++)
   {
      cJSON *f = cJSON_GetArrayItem(jfiles, i);
      if (!cJSON_IsString(f))
         continue;
      char *esc = shell_quote(f->valuestring);
      fpos = str_appendf(file_args, fpos, (int)sizeof(file_args), " %s", esc);
      free(esc);
   }

   char cmd[8192];
   int cpos = snprintf(cmd, sizeof(cmd), "git restore");

   if (staged)
      cpos = str_appendf(cmd, cpos, (int)sizeof(cmd), " --staged");

   if (cJSON_IsString(jsource) && jsource->valuestring[0])
   {
      char *esc = shell_quote(jsource->valuestring);
      cpos = str_appendf(cmd, cpos, (int)sizeof(cmd), " --source=%s", esc);
      free(esc);
   }

   snprintf(cmd + cpos, sizeof(cmd) - (size_t)cpos, "%s 2>&1", file_args);

   int rc;
   char *out = mcp_git_run(cmd, &rc);
   if (rc != 0)
   {
      cJSON *r = mcp_error("error: git restore failed: %s", out ? out : "unknown");
      free(out);
      return r;
   }

   char result[512];
   snprintf(result, sizeof(result), "restored %d file(s)%s", file_count,
            staged ? " (unstaged)" : "");
   free(out);
   return mcp_text(result);
}
