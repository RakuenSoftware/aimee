/* mcp_git_query.c: MCP git read-only handlers and shared state */
#include "aimee.h"
#include "db1/git_ownership.h"
#include "cJSON.h"
#include "headers/config.h"
#include "headers/guardrails.h"
#include "headers/git_verify.h"
#include "headers/mcp_git.h"
#include "headers/platform_process.h"
#include "headers/util.h"
#include "headers/workspace_provider.h"
#include "headers/forge_credentials.h"
#include "headers/git_cred_inject.h"
#include <time.h>

extern char **environ;
#include <dirent.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>

#define GIT_BUF_SIZE 65536
#define SUMMARY_MAX  4096

/* Track whether the current MCP git operation is running in a worktree.
 * Thread-local so concurrent sessions don't clobber each other. */
static __thread int s_in_worktree = 0;

void mcp_git_set_worktree(int val)
{
   s_in_worktree = val;
}

int mcp_git_get_worktree(void)
{
   return s_in_worktree;
}

/* Route a git/gh shell command-line through the turn's active workspace
 * provider. `shared` is a passthrough to run_cmd (identical to today); a
 * `detached` workspace marshals the command to its filesystem authority. */
/* The registered workspace root containing `cwd`, copied into out[outsz].
 * Returns 0 on a match, -1 otherwise. */
static int forge_workspace_for_cwd(const char *cwd, char *out, size_t outsz)
{
   if (!cwd || !cwd[0])
      return -1;
   config_t cfg;
   config_load(&cfg);
   for (int i = 0; i < cfg.workspace_count; i++)
   {
      const char *ws = cfg.workspaces[i];
      size_t len = ws ? strlen(ws) : 0;
      if (len == 0)
         continue;
      if (strncmp(cwd, ws, len) == 0 && (cwd[len] == '/' || cwd[len] == '\0'))
      {
         snprintf(out, outsz, "%s", ws);
         return 0;
      }
   }
   return -1;
}

char *mcp_git_run(const char *cmd, int *exit_code)
{
   const workspace_provider_t *ws = workspace_provider_active();

   /* Credential injection: when the server runs git LOCALLY (shared provider) for
    * a turn whose cwd is inside a registered workspace, run the command under an
    * execve environment carrying GH_TOKEN + the GIT_ASKPASS shim so
    * clone/fetch/push/PR authenticate — never on the command line or disk. The
    * token is resolved through the one shared vault-first policy: a client-handed
    * per-workspace broker token (§4) wins, else the per-host vault token for the
    * checkout's `origin`, else the server's own forge identity (§6); no token →
    * fall through to ambient creds (co-located dev's own gh/SSH). A `detached`
    * workspace marshals git to the client, which holds its own creds, so it stays
    * on the provider path. */
   if (ws->kind == WS_PROVIDER_SHARED)
   {
      const char *cwd = run_cmd_get_cwd();
      char wsid[MAX_PATH_LEN];
      if (cwd && forge_workspace_for_cwd(cwd, wsid, sizeof(wsid)) == 0)
      {
         /* Resolve the git credential through the ONE policy
          * (git_cred_inject_build_env_for_repo) so the precedence never drifts
          * from the other call sites: a client-handed per-workspace broker token
          * (§4) is passed as preferred_token and wins, else per-host server vault
          * → server identity → ambient. The policy injects GH_TOKEN + the
          * GIT_ASKPASS shim and wipes its own token copy. */
         char tok[4096] = {0};
         const char *pref = NULL;
         if (forge_cred_get(wsid, (long)time(NULL), tok, sizeof(tok)) == 0 && tok[0])
            pref = tok;
         char **envp = git_cred_inject_build_env_for_repo(NULL, NULL, cwd, pref, environ, NULL);
         volatile char *p = (volatile char *)tok;
         for (size_t i = 0; i < sizeof(tok); i++)
            p[i] = 0;
         if (envp)
         {
            char *out = run_cmd_env(cmd, envp, exit_code);
            git_cred_inject_free_env(envp);
            return out;
         }
      }
   }
   return ws->exec_shell(ws, cmd, exit_code);
}

static void trim_trailing_newline(char *s)
{
   if (!s)
      return;
   size_t len = strlen(s);
   while (len > 0 && (s[len - 1] == '\n' || s[len - 1] == '\r'))
      s[--len] = '\0';
}

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

static int mcp_git_candidate_root(const char *candidate, char *git_root, size_t git_root_len)
{
   if (!candidate || !candidate[0] || !git_root || git_root_len == 0)
      return -1;

   git_root[0] = '\0';

   char git_cmd[MAX_PATH_LEN + 64];
   char *esc = shell_escape(candidate);
   snprintf(git_cmd, sizeof(git_cmd), "git -C %s rev-parse --show-toplevel 2>/dev/null", esc);
   free(esc);
   int rc;
   char *out = mcp_git_run(git_cmd, &rc);
   if (rc == 0 && out && out[0])
   {
      trim_trailing_newline(out);
      if (out[0])
      {
         snprintf(git_root, git_root_len, "%s", out);
         free(out);
         return 0;
      }
   }
   free(out);

   /* Fallback for launcher directories that contain checked-out projects.
    * Skip filesystem root to avoid an expensive and low-signal scan of /. */
   if (strcmp(candidate, "/") == 0)
      return -1;

   DIR *d = opendir(candidate);
   if (!d)
      return -1;

   struct dirent *ent;
   while ((ent = readdir(d)) != NULL)
   {
      if (ent->d_name[0] == '.')
         continue;
      char subdir[MAX_PATH_LEN];
      snprintf(subdir, sizeof(subdir), "%s/%s", candidate, ent->d_name);
      struct stat st;
      if (stat(subdir, &st) != 0 || !S_ISDIR(st.st_mode))
         continue;
      char probe[MAX_PATH_LEN];
      snprintf(probe, sizeof(probe), "%s/.git", subdir);
      if (access(probe, F_OK) != 0)
         continue;
      snprintf(probe, sizeof(probe), "%s/.aimee/project.yaml", subdir);
      if (access(probe, F_OK) != 0)
         continue;
      snprintf(git_root, git_root_len, "%s", subdir);
      closedir(d);
      return 0;
   }
   closedir(d);
   return -1;
}

static void mcp_git_add_candidate(char candidates[][MAX_PATH_LEN], int *count, int cap,
                                  const char *candidate)
{
   if (!candidates || !count || *count >= cap || !candidate || !candidate[0])
      return;
   for (int i = 0; i < *count; i++)
      if (strcmp(candidates[i], candidate) == 0)
         return;
   snprintf(candidates[*count], MAX_PATH_LEN, "%s", candidate);
   (*count)++;
}

static int mcp_git_replace_stale_delegate_cwd(const char *tracked, char *out, size_t out_len)
{
   if (!tracked || !tracked[0] || !out || out_len == 0)
      return 0;
   const char *marker = strstr(tracked, "/.aimee/worktrees/");
   if (!marker || marker == tracked)
      return 0;
   const char *slot = marker + strlen("/.aimee/worktrees/");
   if (strncmp(slot, "deleg-", 6) != 0)
      return 0;

   const char *sid = session_id();
   if (!sid || !sid[0] || strncmp(sid, "deleg-", 6) == 0)
      return 0;

   /* Delegate worktrees are transient; a stale git-cwd entry should not anchor
    * later MCP calls when the owning session main worktree is still present. */
   size_t root_len = (size_t)(marker - tracked);
   if (root_len == 0 || root_len >= out_len)
      return 0;
   char root[MAX_PATH_LEN];
   memcpy(root, tracked, root_len);
   root[root_len] = '\0';

   char candidate[MAX_PATH_LEN];
   if (worktree_sibling_path(root, sid, NULL, candidate, sizeof(candidate)) != 0)
      return 0;
   struct stat st;
   if (stat(candidate, &st) != 0 || !S_ISDIR(st.st_mode))
      return 0;
   snprintf(out, out_len, "%s", candidate);
   return 1;
}

static int mcp_git_is_managed_worktree_root(const char *path)
{
   if (!path || !path[0])
      return 0;
   const char *marker = strstr(path, "/.aimee/worktrees/");
   if (!marker)
      return 0;
   const char *slot = marker + strlen("/.aimee/worktrees/");
   return slot[0] != '\0' && strchr(slot, '/') != NULL;
}

/* --- Merged-PR detection --- */

/* Check if the given branch has a merged PR. Returns 1 if merged, 0 if not.
 * Skips the check for main/master. */
int check_branch_has_merged_pr_for(const char *branch)
{
   if (!branch || !branch[0])
      return 0;
   if (strcmp(branch, "main") == 0 || strcmp(branch, "master") == 0)
      return 0;

   char cmd[512];
   snprintf(cmd, sizeof(cmd),
            "gh pr list --head '%s' --state merged --json number --limit 1 2>/dev/null", branch);

   int rc;
   char *out = mcp_git_run(cmd, &rc);
   if (rc != 0 || !out)
   {
      free(out);
      return 0; /* gh not available or failed -- don't block */
   }

   int has_merged = (strstr(out, "\"number\"") != NULL);
   free(out);
   return has_merged;
}

/* --- Branch helper --- */

/* Get the current branch name. Returns 0 on success, -1 on failure.
 * Writes branch name to buf. */
int get_current_branch(char *buf, size_t len)
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

/* --- git_status --- */

cJSON *handle_git_status(cJSON *args)
{
   (void)args;
   int rc;
   /* --short --branch: first line is "## branch...upstream [ahead N, behind M]",
    * subsequent lines are "XY filename" entries. Simpler to parse than porcelain v2. */
   char *raw = mcp_git_run("git status --short --branch 2>&1", &rc);
   if (!raw)
      return mcp_text("error: failed to run git status");

   if (rc != 0)
   {
      cJSON *r = mcp_error("error: git status failed: %s", raw);
      free(raw);
      return r;
   }

   char branch[256] = "unknown";
   char tracking[64] = ""; /* "(up to date)", "[ahead N]", etc. */
   int staged = 0, modified = 0, untracked = 0, conflicted = 0;
   char staged_files[2048] = "", modified_files[2048] = "", untracked_files[2048] = "";
   size_t sf_len = 0, mf_len = 0, uf_len = 0;

   char *line = raw;
   int first = 1;
   while (line && *line)
   {
      char *nl = strchr(line, '\n');
      if (nl)
         *nl = '\0';

      if (first)
      {
         first = 0;
         /* "## branch...upstream [ahead N, behind M]" or "## branch" */
         if (strncmp(line, "## ", 3) == 0)
         {
            const char *p = line + 3;
            const char *dots = strstr(p, "...");
            const char *bracket = strchr(p, '[');
            if (dots)
               snprintf(branch, sizeof(branch), "%.*s", (int)(dots - p), p);
            else
               snprintf(branch, sizeof(branch), "%s", bracket ? "" : p);
            if (bracket && strchr(bracket, ']'))
               snprintf(tracking, sizeof(tracking), "%.*s",
                        (int)(strchr(bracket, ']') - bracket + 1), bracket);
            else if (dots)
               snprintf(tracking, sizeof(tracking), "(up to date)");
            else
               snprintf(tracking, sizeof(tracking), "(no upstream)");
         }
      }
      else if (line[0] && line[1])
      {
         char x = line[0], y = line[1];
         const char *fname = (line[2] == ' ') ? line + 3 : line + 2;
         /* Renames: "old -> new" — show only the destination */
         const char *arrow = strstr(fname, " -> ");
         if (arrow)
            fname = arrow + 4;

         if (x == '?' && y == '?')
         {
            untracked++;
            if (uf_len < sizeof(untracked_files) - 200)
               uf_len +=
                   (size_t)snprintf(untracked_files + uf_len, sizeof(untracked_files) - uf_len,
                                    "%s%s", uf_len ? ", " : "", fname);
         }
         else if (x == 'U' || y == 'U' || (x == 'A' && y == 'A') || (x == 'D' && y == 'D'))
         {
            conflicted++;
         }
         else
         {
            if (x != ' ')
            {
               staged++;
               if (sf_len < sizeof(staged_files) - 200)
                  sf_len += (size_t)snprintf(staged_files + sf_len, sizeof(staged_files) - sf_len,
                                             "%s%s", sf_len ? ", " : "", fname);
            }
            if (y != ' ')
            {
               modified++;
               if (mf_len < sizeof(modified_files) - 200)
                  mf_len +=
                      (size_t)snprintf(modified_files + mf_len, sizeof(modified_files) - mf_len,
                                       "%s%s", mf_len ? ", " : "", fname);
            }
         }
      }

      line = nl ? nl + 1 : NULL;
   }
   free(raw);

   char out[SUMMARY_MAX];
   int pos = 0;
   pos = str_appendf(out, pos, (int)sizeof(out), "branch: %s", branch);
   if (tracking[0])
      pos = str_appendf(out, pos, (int)sizeof(out), " %s", tracking);

   if (staged)
      pos = str_appendf(out, pos, (int)sizeof(out), "\nstaged: %d file%s (%s)", staged,
                        staged == 1 ? "" : "s", staged_files);
   if (modified)
      pos = str_appendf(out, pos, (int)sizeof(out), "\nmodified: %d file%s (%s)", modified,
                        modified == 1 ? "" : "s", modified_files);
   if (untracked)
      pos = str_appendf(out, pos, (int)sizeof(out), "\nuntracked: %d file%s (%s)", untracked,
                        untracked == 1 ? "" : "s", untracked_files);
   if (conflicted)
      pos = str_appendf(out, pos, (int)sizeof(out), "\nconflicted: %d file%s", conflicted,
                        conflicted == 1 ? "" : "s");
   if (!staged && !modified && !untracked && !conflicted)
      pos = str_appendf(out, pos, (int)sizeof(out), "\nclean working tree");

   (void)pos;
   return mcp_text(out);
}

/* --- git_log --- */

cJSON *handle_git_log(cJSON *args)
{
   cJSON *jcount = cJSON_GetObjectItemCaseSensitive(args, "count");
   cJSON *jref = cJSON_GetObjectItemCaseSensitive(args, "ref");
   cJSON *jstat = cJSON_GetObjectItemCaseSensitive(args, "diff_stat");

   int count = 10;
   if (cJSON_IsNumber(jcount) && jcount->valueint > 0 && jcount->valueint <= 50)
      count = jcount->valueint;

   const char *stat_flag = (jstat && cJSON_IsTrue(jstat)) ? " --stat" : "";

   char cmd[1024];
   if (cJSON_IsString(jref) && jref->valuestring[0])
   {
      char *esc = shell_escape(jref->valuestring);
      snprintf(cmd, sizeof(cmd), "git log --format='%%h %%ar  %%s'%s -n %d '%s' 2>&1", stat_flag,
               count, esc);
      free(esc);
   }
   else
   {
      snprintf(cmd, sizeof(cmd), "git log --format='%%h %%ar  %%s'%s -n %d 2>&1", stat_flag, count);
   }

   int rc;
   char *out = mcp_git_run(cmd, &rc);
   if (rc != 0)
   {
      cJSON *r = mcp_error("error: git log failed: %s", out ? out : "unknown");
      free(out);
      return r;
   }

   /* Truncate if very long */
   if (out && strlen(out) > 4000)
   {
      out[4000] = '\0';
      size_t len = strlen(out);
      snprintf(out + len, GIT_BUF_SIZE - len, "\n... (truncated)");
   }

   cJSON *r = mcp_text(out && out[0] ? out : "(no commits)");
   free(out);
   return r;
}

/* --- git_diff_summary --- */

cJSON *handle_git_diff_summary(cJSON *args)
{
   cJSON *jref = cJSON_GetObjectItemCaseSensitive(args, "ref");
   cJSON *jstat = cJSON_GetObjectItemCaseSensitive(args, "stat_only");
   cJSON *jfiles = cJSON_GetObjectItemCaseSensitive(args, "files");

   int stat_only = 1; /* default true */
   if (jstat && cJSON_IsFalse(jstat))
      stat_only = 0;

   /* Build command */
   char cmd[4096];
   size_t cmd_len = 0;
   int n;

   if (stat_only)
   {
      if (cJSON_IsString(jref) && jref->valuestring[0])
      {
         char *esc = shell_escape(jref->valuestring);
         n = snprintf(cmd, sizeof(cmd), "git diff --stat '%s'", esc);
         free(esc);
      }
      else
      {
         n = snprintf(cmd, sizeof(cmd), "git diff --stat");
      }
   }
   else
   {
      /* Full diff but we'll compress it */
      if (cJSON_IsString(jref) && jref->valuestring[0])
      {
         char *esc = shell_escape(jref->valuestring);
         n = snprintf(cmd, sizeof(cmd), "git diff '%s'", esc);
         free(esc);
      }
      else
      {
         n = snprintf(cmd, sizeof(cmd), "git diff");
      }
   }
   cmd_len = (n >= 0 && (size_t)n < sizeof(cmd)) ? (size_t)n : sizeof(cmd) - 1;

   /* Append file filters */
   if (jfiles && cJSON_IsArray(jfiles) && cJSON_GetArraySize(jfiles) > 0)
   {
      n = snprintf(cmd + cmd_len, sizeof(cmd) - cmd_len, " --");
      if (n > 0 && cmd_len + (size_t)n < sizeof(cmd))
         cmd_len += (size_t)n;
      int fcount = cJSON_GetArraySize(jfiles);
      for (int i = 0; i < fcount && i < 20; i++)
      {
         cJSON *f = cJSON_GetArrayItem(jfiles, i);
         if (cJSON_IsString(f))
         {
            char *esc = shell_escape(f->valuestring);
            n = snprintf(cmd + cmd_len, sizeof(cmd) - cmd_len, " '%s'", esc);
            free(esc);
            if (n < 0 || cmd_len + (size_t)n >= sizeof(cmd))
               break;
            cmd_len += (size_t)n;
         }
      }
   }

   if (cmd_len + 6 < sizeof(cmd))
      memcpy(cmd + cmd_len, " 2>&1", 6);

   int rc;
   char *out = mcp_git_run(cmd, &rc);
   if (rc != 0)
   {
      cJSON *r = mcp_error("error: git diff failed: %s", out ? out : "unknown");
      free(out);
      return r;
   }

   if (!out || !out[0])
   {
      free(out);
      return mcp_text("no changes");
   }

   if (stat_only)
   {
      /* Already compact, just truncate if needed */
      if (strlen(out) > 3000)
      {
         out[3000] = '\0';
         size_t len = strlen(out);
         snprintf(out + len, GIT_BUF_SIZE - len, "\n... (truncated)");
      }
      cJSON *r = mcp_text(out);
      free(out);
      return r;
   }

   /* Compress full diff to change descriptions per file.
    * Include hunk headers and changed lines (truncated per file) so the
    * caller can understand *what* changed, not just how many lines. */
   char summary[SUMMARY_MAX];
   int spos = 0;
   char current_file[512] = "";
   int file_adds = 0, file_dels = 0;
   int file_count = 0;
   int file_lines = 0;                       /* changed lines emitted for current file */
   static const int MAX_LINES_PER_FILE = 40; /* cap per file to keep summary compact */

   char *line = out;
   while (line && *line && spos < SUMMARY_MAX - 200)
   {
      char *nl = strchr(line, '\n');
      if (nl)
         *nl = '\0';

      if (strncmp(line, "diff --git ", 11) == 0)
      {
         /* Flush previous file header */
         if (current_file[0] && file_count <= 20)
         {
            if (file_lines >= MAX_LINES_PER_FILE)
               spos = str_appendf(summary, spos, (int)sizeof(summary),
                                  "  ... (%d more changed lines)\n",
                                  (file_adds + file_dels) - file_lines);
         }
         /* Parse new file: "diff --git a/foo b/foo" -> "foo" */
         const char *b = strstr(line, " b/");
         if (b)
            snprintf(current_file, sizeof(current_file), "%s", b + 3);
         else
            snprintf(current_file, sizeof(current_file), "%s", line + 11);
         file_adds = 0;
         file_dels = 0;
         file_lines = 0;
         file_count++;
         if (file_count <= 20)
            spos = str_appendf(summary, spos, (int)sizeof(summary), "%s\n", current_file);
      }
      else if (strncmp(line, "@@ ", 3) == 0 && file_count <= 20)
      {
         /* Hunk header — include it for context */
         spos = str_appendf(summary, spos, (int)sizeof(summary), "  %s\n", line);
         file_lines = 0; /* reset per-hunk counter so each hunk gets some lines */
      }
      else if ((line[0] == '+' && line[1] != '+') || (line[0] == '-' && line[1] != '-'))
      {
         if (line[0] == '+')
            file_adds++;
         else
            file_dels++;
         if (file_count <= 20 && file_lines < MAX_LINES_PER_FILE)
         {
            /* Truncate very long lines */
            if (strlen(line) > 120)
               spos +=
                   snprintf(summary + spos, sizeof(summary) - (size_t)spos, "  %.120s...\n", line);
            else
               spos = str_appendf(summary, spos, (int)sizeof(summary), "  %s\n", line);
            file_lines++;
         }
      }

      line = nl ? nl + 1 : NULL;
   }

   /* Flush last file overflow */
   if (current_file[0] && file_count <= 20 && file_lines >= MAX_LINES_PER_FILE)
   {
      spos = str_appendf(summary, spos, (int)sizeof(summary), "  ... (%d more changed lines)\n",
                         (file_adds + file_dels) - file_lines);
   }

   if (file_count > 20)
      spos = str_appendf(summary, spos, (int)sizeof(summary), "... and %d more files\n",
                         file_count - 20);

   (void)spos;
   free(out);
   return mcp_text(summary[0] ? summary : "no changes");
}

/* --- git_issue --- */

cJSON *handle_git_issue(cJSON *args)
{
   cJSON *jaction = cJSON_GetObjectItemCaseSensitive(args, "action");
   const char *action =
       (cJSON_IsString(jaction) && jaction->valuestring[0]) ? jaction->valuestring : "list";

   if (strcmp(action, "list") == 0)
   {
      cJSON *jstate = cJSON_GetObjectItemCaseSensitive(args, "state");
      const char *state =
          (cJSON_IsString(jstate) && jstate->valuestring[0]) ? jstate->valuestring : "open";

      /* Validate state value */
      if (strcmp(state, "open") != 0 && strcmp(state, "closed") != 0 && strcmp(state, "all") != 0)
         return mcp_text("error: 'state' must be one of: open, closed, all");

      char cmd[512];
      snprintf(cmd, sizeof(cmd),
               "gh issue list --limit 30 --state %s "
               "--json number,title,state,labels "
               "--template '{{range .}}#{{.number}} [{{.state}}] {{.title}}"
               "{{if .labels}} ({{range .labels}}{{.name}} {{end}}){{end}}\n{{end}}' "
               "2>&1",
               state);
      int rc;
      char *out = mcp_git_run(cmd, &rc);
      if (rc != 0)
      {
         cJSON *r = mcp_error("error: gh issue list failed: %s", out ? out : "unknown");
         free(out);
         return r;
      }
      cJSON *r = mcp_text(out && out[0] ? out : "(no issues)");
      free(out);
      return r;
   }

   return mcp_text("error: unknown action. Use list");
}

/* --- CWD helper for git tools --- */

int mcp_chdir_git_root(char *old_cwd, size_t old_cwd_len, cJSON *args, char **mismatch_err)
{
   if (mismatch_err)
      *mismatch_err = NULL;

   /* old_cwd kept for API compat; no longer saves/restores process CWD */
   if (old_cwd && old_cwd_len > 0)
      old_cwd[0] = '\0';

   /* Long-lived MCP processes cache session_id at first use; a session
    * rotation (`aimee session-start` between requests) would leave that
    * cache pointing at a stale session whose worktree mapping is dead.
    * Drop the cache so the next session_id() call re-reads the PPID file. */
   session_id_refresh();

   /* Always reset first so a failed resolution leaves no stale directory.
    * After this point, mcp_git_run() calls in this function use process CWD
    * (no tl_run_cwd prefix) which is safe during resolution. */
   run_cmd_set_cwd(NULL);
   s_in_worktree = 0;

   char candidates[8][MAX_PATH_LEN];
   int candidate_count = 0;
   int no_session_redirect =
       args && cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(args, "no_session_redirect"));

   /* Priority 1: explicit 'path' argument from tool call */
   if (args)
   {
      cJSON *jpath = cJSON_GetObjectItemCaseSensitive(args, "path");
      if (cJSON_IsString(jpath) && jpath->valuestring[0])
         mcp_git_add_candidate(candidates, &candidate_count, 8, jpath->valuestring);
   }

   /* Priority 2: session CWD tracking file (thread-safe: keyed by session_id) */
   if (!no_session_redirect)
   {
      char cwd_path[MAX_PATH_LEN];
      snprintf(cwd_path, sizeof(cwd_path), "%s/git-cwd-%s", config_output_dir(), session_id());
      FILE *fp = fopen(cwd_path, "r");
      if (fp)
      {
         char tracked[MAX_PATH_LEN] = "";
         if (fgets(tracked, sizeof(tracked), fp))
         {
            size_t len = strlen(tracked);
            while (len > 0 && (tracked[len - 1] == '\n' || tracked[len - 1] == '\r'))
               tracked[--len] = '\0';
            if (tracked[0])
            {
               char repaired[MAX_PATH_LEN];
               if (mcp_git_replace_stale_delegate_cwd(tracked, repaired, sizeof(repaired)))
                  mcp_git_add_candidate(candidates, &candidate_count, 8, repaired);
               else
                  mcp_git_add_candidate(candidates, &candidate_count, 8, tracked);
            }
         }
         fclose(fp);
      }
   }

   /* Priority 3: caller cwd. MCP stdio proxies may be long-lived and launched
    * from a stale directory, so this must not outrank the session cwd file. */
   if (args)
   {
      cJSON *jcwd = cJSON_GetObjectItemCaseSensitive(args, "cwd");
      if (cJSON_IsString(jcwd) && jcwd->valuestring[0])
         mcp_git_add_candidate(candidates, &candidate_count, 8, jcwd->valuestring);
   }

   /* Priority 4: environment fallbacks. Some MCP hosts launch the stdio
    * wrapper from / but preserve a meaningful PWD or explicit override. */
   {
      mcp_git_add_candidate(candidates, &candidate_count, 8, getenv("AIMEE_MCP_CWD"));
      mcp_git_add_candidate(candidates, &candidate_count, 8, getenv("PWD"));
   }

   /* Priority 5: process CWD (racy in multi-session but useful as a fallback) */
   {
      char cwd_buf[MAX_PATH_LEN];
      if (getcwd(cwd_buf, sizeof(cwd_buf)))
         mcp_git_add_candidate(candidates, &candidate_count, 8, cwd_buf);
   }

   /* Priority 6: executable directory. This catches repo-local plugin/server
    * launches where the process cwd is not the checkout root. */
   {
      char exe[MAX_PATH_LEN];
      if (platform_get_exe_path(exe, sizeof(exe)) == 0)
      {
         char *slash = strrchr(exe, '/');
         if (slash && slash != exe)
         {
            *slash = '\0';
            mcp_git_add_candidate(candidates, &candidate_count, 8, exe);
         }
      }
   }

   char git_root[MAX_PATH_LEN] = "";
   char resolved_dir[MAX_PATH_LEN] = "";
   for (int i = 0; i < candidate_count; i++)
   {
      if (mcp_git_candidate_root(candidates[i], git_root, sizeof(git_root)) == 0)
      {
         snprintf(resolved_dir, sizeof(resolved_dir), "%s", candidates[i]);
         break;
      }
   }

   if (!git_root[0])
      return 0; /* No git repo found; run_cmd uses process CWD */

   /* Worktree redirect: if session has a sibling worktree for this git root,
    * point run_cmd there instead. This is the single place where worktree
    * redirection happens for all git MCP handlers. */
   {
      const char *sid = session_id();
      if (!no_session_redirect && sid && sid[0] && !mcp_git_is_managed_worktree_root(git_root))
      {
         session_state_t state;
         session_state_load(&state, sid);

         if (state.worktree_count > 0)
         {
            const char *wt = worktree_for_cwd(&state, git_root);
            if (wt && wt[0])
            {
               struct stat st;
               if (stat(wt, &st) == 0 && S_ISDIR(st.st_mode))
               {
                  if (mismatch_err && strcmp(git_root, wt) != 0)
                  {
                     char cmd_root[MAX_PATH_LEN + 64];
                     char cmd_wt[MAX_PATH_LEN + 64];
                     char *esc_root = shell_escape(git_root);
                     char *esc_wt = shell_escape(wt);
                     snprintf(cmd_root, sizeof(cmd_root),
                              "git -C %s rev-parse --abbrev-ref HEAD 2>/dev/null", esc_root);
                     snprintf(cmd_wt, sizeof(cmd_wt),
                              "git -C %s rev-parse --abbrev-ref HEAD 2>/dev/null", esc_wt);
                     free(esc_root);
                     free(esc_wt);

                     int rc_root = 0, rc_wt = 0;
                     char *branch_root = mcp_git_run(cmd_root, &rc_root);
                     char *branch_wt = mcp_git_run(cmd_wt, &rc_wt);

                     if (rc_root == 0 && rc_wt == 0 && branch_root && branch_wt)
                     {
                        size_t lr = strlen(branch_root);
                        if (lr > 0 && (branch_root[lr - 1] == '\n' || branch_root[lr - 1] == '\r'))
                           branch_root[lr - 1] = '\0';
                        size_t lw = strlen(branch_wt);
                        if (lw > 0 && (branch_wt[lw - 1] == '\n' || branch_wt[lw - 1] == '\r'))
                           branch_wt[lw - 1] = '\0';

                        size_t msg_len = 1024;
                        *mismatch_err = malloc(msg_len);
                        snprintf(*mismatch_err, msg_len,
                                 "Context Mismatch: The MCP git tool is bound to a different "
                                 "worktree/branch than your active checkout.\n"
                                 "  Active Checkout : %s (branch: %s)\n"
                                 "  MCP Session     : %s (branch: %s)\n\n"
                                 "Use the 'aimee work' or 'aimee branch' commands to align the "
                                 "session, or 'aimee wm' to clear state.",
                                 git_root, branch_root, wt, branch_wt);
                     }
                     free(branch_root);
                     free(branch_wt);
                  }

                  /* The worktree path encodes the first 8 chars of the session UUID that
                   * created it (e.g. .aimee-web-403328b1).  The MCP request may carry a
                   * different session_id (e.g. after a Claude Code restart), so look up
                   * the full UUID from the DB and override — otherwise branch_ownership
                   * queries will use the wrong session and find a stale branch. */
                  {
                     const char *wt_base = strrchr(wt, '/');
                     wt_base = wt_base ? wt_base + 1 : wt;
                     /* Find the rightmost '-<8hexchars>' segment */
                     char short_id[12] = "";
                     for (const char *q = wt_base; *q; q++)
                     {
                        if (*q == '-' && strlen(q + 1) >= 8)
                        {
                           int ok = 1;
                           for (int j = 0; j < 8; j++)
                              if (!isxdigit((unsigned char)q[1 + j]))
                              {
                                 ok = 0;
                                 break;
                              }
                           if (ok && (q[9] == '-' || q[9] == '\0'))
                              snprintf(short_id, sizeof(short_id), "%.8s", q + 1);
                        }
                     }
                     if (short_id[0])
                     {
                        char full_sid[64];
                        if (db1_git_ownership_find_session_by_prefix(short_id, full_sid,
                                                                     sizeof(full_sid)) == 1 &&
                            full_sid[0])
                           session_id_set_override(full_sid);
                     }
                  }
                  run_cmd_set_cwd(wt);
                  s_in_worktree = 1;
                  return 1;
               }
               /* Worktree expected but inaccessible — abort to avoid wrong repo */
               return -1;
            }
         }
      }
   }

   /* Non-worktree session override: if the current HEAD branch is owned by a
    * different session (e.g. after a Claude Code restart or stdin-pipe re-invocation),
    * adopt that session so branch_ownership checks pass.  Parallel to the worktree
    * session override block above. */
   {
      /* Derive the canonical repo path using -C to avoid relying on tl_run_cwd. */
      char repo_path[MAX_PATH_LEN] = "";
      {
         char cmd[MAX_PATH_LEN + 64];
         char *esc = shell_escape(git_root);
         snprintf(cmd, sizeof(cmd), "git -C %s rev-parse --git-common-dir 2>/dev/null", esc);
         free(esc);
         int rc;
         char *out = mcp_git_run(cmd, &rc);
         if (rc == 0 && out && out[0])
         {
            /* Strip trailing newline */
            size_t olen = strlen(out);
            while (olen > 0 && (out[olen - 1] == '\n' || out[olen - 1] == '\r'))
               out[--olen] = '\0';
            if (strcmp(out, ".git") != 0 && out[0] == '/')
            {
               /* Absolute path (worktree) — strip "/.git..." suffix */
               char *git_suffix = strstr(out, "/.git");
               if (git_suffix)
               {
                  *git_suffix = '\0';
                  snprintf(repo_path, sizeof(repo_path), "%s", out);
               }
            }
            else
            {
               /* Relative ".git" means regular checkout — use git_root directly */
               snprintf(repo_path, sizeof(repo_path), "%s", git_root);
            }
         }
         else
         {
            snprintf(repo_path, sizeof(repo_path), "%s", git_root);
         }
         free(out);
      }

      /* Get the current HEAD branch name */
      char branch_name[256] = "";
      {
         char cmd[MAX_PATH_LEN + 64];
         char *esc = shell_escape(git_root);
         snprintf(cmd, sizeof(cmd), "git -C %s rev-parse --abbrev-ref HEAD 2>/dev/null", esc);
         free(esc);
         int rc;
         char *out = mcp_git_run(cmd, &rc);
         if (rc == 0 && out && out[0])
         {
            size_t olen = strlen(out);
            while (olen > 0 && (out[olen - 1] == '\n' || out[olen - 1] == '\r'))
               out[--olen] = '\0';
            /* "HEAD" means detached — no ownership record to look up */
            if (olen > 0 && strcmp(out, "HEAD") != 0)
               snprintf(branch_name, sizeof(branch_name), "%s", out);
         }
         free(out);
      }

      /* Look up ownership and override session if needed */
      if (repo_path[0] && branch_name[0])
      {
         char owner[64];
         if (db1_git_ownership_get_owner(repo_path, branch_name, owner, sizeof(owner)) == 1)
         {
            const char *cur_sid = session_id();
            if (owner[0] && cur_sid && strcmp(owner, cur_sid) != 0)
               session_id_set_override(owner);
         }
      }
   }

   run_cmd_set_cwd(git_root);
   return 1;
}
