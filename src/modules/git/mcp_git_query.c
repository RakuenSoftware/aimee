/* mcp_git_query.c: MCP git read-only handlers and shared state */
#include "aimee.h"
#include "db1/git_ownership.h"
#include "cJSON.h"
#include "config.h"
#include "guardrails.h"
#include "git_verify.h"
#include "log.h"
#include "mcp_git.h"
#include "platform_process.h"
#include "util.h"
#include "modules/workspace/workspace_provider.h"
#include "headers/module_json_call.h"
#include "forge_credentials.h"
#include "git_cred_inject.h"
#include "aimee_home.h"
#include <aimee/git/module_api.h>
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

#define GIT_CRED_RESOLVE_MAX_BODY   (256u * 1024u)
#define GIT_CRED_RESOLVE_TIMEOUT_MS 5000

/* Ask the git module where this command runs, and which workspace owns its cwd.
 *
 * Both are DECISIONS, so neither is taken here. This carries the facts the
 * module cannot see — the active workspace's provider kind, the registered
 * roots, and the mirror base, whose resolution reads the environment and the
 * instance home — and applies the ruling. The module is server-go/modules/git
 * (cred_resolve.go), reached as bus stage AIMEE_GIT_STAGE_CRED_RESOLVE.
 *
 * Deciding it here is what broke. The server-run test was a LIST of provider
 * kinds that omitted `mirror`, and the cwd was prefix-matched against the
 * registered root — which for a mirror workspace is the CLIENT's path, a
 * directory that does not exist on this server, while git runs in a
 * reconstruction elsewhere. Both said "not ours" about a live checkout, so the
 * credential below was never injected and git ran bare: "could not read
 * Username", indistinguishable from a dead token, while reads kept working
 * because git_pr_api.c never execs git.
 *
 * On a module failure `*runs_on_server` stays 1 and no workspace is reported, so
 * the command still runs here with no injected credential — the same ambient
 * fall-through already documented for "no token", rather than a new failure
 * mode. Returns 0 with `out` set when a workspace owns the cwd, -1 otherwise. */
/* The remote URL the registry records for the workspace rooted at `root`, or
 * NULL when unknown. Read from the same accessor the mirror lifecycle uses
 * (workspace_turn.c), so the two cannot disagree about a workspace's remote.
 * Returns a pointer into config storage: use it before any further config read.
 */
static const char *workspace_remote_for_root(const char *root)
{
   if (!root || !root[0])
      return NULL;
   for (int i = 0; i < config_workspace_count(); i++)
   {
      const char *candidate = config_workspaces(i);
      if (candidate && strcmp(candidate, root) == 0)
      {
         const char *remote = config_workspace_vcs_remote(i);
         return (remote && remote[0]) ? remote : NULL;
      }
   }
   return NULL;
}

static int forge_workspace_for_cwd(const char *cwd, int provider_kind, int *runs_on_server,
                                   char *out, size_t outsz)
{
   if (out && outsz)
      out[0] = '\0';
   if (runs_on_server)
      *runs_on_server = 1;

   cJSON *request = cJSON_CreateObject();
   if (!request)
      return -1;
   cJSON_AddStringToObject(request, "cwd", cwd ? cwd : "");
   cJSON_AddNumberToObject(request, "provider_kind", provider_kind);
   /* The instance home, not a mirror path: resolving where a provider puts its
    * trees is the module's job, and reaching into the workspace module's headers
    * to ask would be the cross-module coupling the bus exists to replace. */
   const char *home = aimee_home();
   cJSON_AddStringToObject(request, "aimee_home", home ? home : "");
   cJSON *roots = cJSON_AddArrayToObject(request, "workspaces");
   for (int i = 0; roots && i < config_workspace_count(); i++)
   {
      const char *root = config_workspaces(i);
      if (root && root[0])
         cJSON_AddItemToArray(roots, cJSON_CreateString(root));
   }

   cJSON *reply =
       aimee_module_json_call(AIMEE_GIT_EVENT_CRED_RESOLVE, AIMEE_GIT_STAGE_CRED_RESOLVE, request,
                              GIT_CRED_RESOLVE_MAX_BODY, GIT_CRED_RESOLVE_TIMEOUT_MS, NULL);
   if (!reply)
      return -1;
   const cJSON *on_server = cJSON_GetObjectItemCaseSensitive(reply, "runs_on_server");
   if (runs_on_server && cJSON_IsBool(on_server))
      *runs_on_server = cJSON_IsTrue(on_server) ? 1 : 0;
   const cJSON *owner = cJSON_GetObjectItemCaseSensitive(reply, "workspace");
   int found = 0;
   if (cJSON_IsString(owner) && owner->valuestring[0] && out && outsz)
   {
      snprintf(out, outsz, "%s", owner->valuestring);
      found = 1;
   }
   cJSON_Delete(reply);
   return found ? 0 : -1;
}

char *mcp_git_run(const char *cmd, int *exit_code)
{
   const workspace_provider_t *ws = workspace_provider_active();

   /* aimee's git tooling is TRUSTED server code and must run where the forge
    * credential, the network and the git rails live: on aimee-server — NOT inside a
    * delegate's sandbox. A CONTAINER-sandboxed delegate runs `--network none` on a
    * minimal image with no git binary and no credential, so routing git into it
    * would (a) fail outright — `git: command not found` — and (b) be wrong: push/PR
    * need the network the sandbox deliberately removes, and running git there would
    * push the forge credential into the sandbox this design exists to keep it out of.
    * The delegate's worktree is bind-mounted path-identically, so the server sees the
    * delegate's edits at the same path. Which kinds that covers is the module's
    * ruling, not a list kept here — see forge_workspace_for_cwd. */
   const char *cwd = run_cmd_get_cwd();
   char wsid[MAX_PATH_LEN];
   int run_on_server = 1;
   int have_ws = forge_workspace_for_cwd(cwd, (int)ws->kind, &run_on_server, wsid, sizeof(wsid));
   const workspace_provider_t *exec_ws = run_on_server ? workspace_provider_shared() : ws;

   /* Credential injection: for a server-run command whose cwd is inside a registered
    * workspace, run under an execve environment that authenticates git — never on the
    * command line or disk. The token is resolved through the one shared vault-first
    * policy: the per-host vault token for the workspace's remote (or the checkout's
    * `origin`), else the principal's vaulted forge token, else the server's own forge
    * identity (§6); no token → fall through to ambient creds (co-located dev's own
    * gh/SSH). No workspace broker token: aimee git proxies through aimee's own
    * vaulted credential, and a brokered one would outrank it.
    *
    * This carries AIMEE_GIT_TOKEN_FD and the GIT_ASKPASS shim, NOT GH_TOKEN. The
    * builder is called in FD mode (out_token_fd non-NULL), which puts the secret on a
    * memfd so it never lands in the child's /proc/<pid>/environ — see
    * git_cred_inject.h. That authenticates `git`, whose askpass reads the fd, and it
    * does NOT authenticate `gh`, which can only take a token from the environment or
    * its own config. So a `gh` subcommand routed through here runs with NO credential
    * and reports "gh auth login", however well the vault is populated. That is why
    * the PR ops read the GitHub API in-process (git_pr_api.c) instead: the raw token
    * goes straight into an Authorization header and no child is involved.
    *
    * Said plainly because the previous wording claimed GH_TOKEN was injected, which
    * sent at least one reader hunting for a broken OAuth that was never broken. */
   if (run_on_server)
   {
      /* The other half of the same silence: with no workspace owning the cwd the
       * block below never runs, so git execs bare and no credential is even
       * attempted. That is the condition this file's header describes as having
       * caused exactly this bug once, and it is indistinguishable from a bad
       * token unless it says so. It is also what a broken credential-resolve
       * stage looks like from here — that stage going unadvertised silently
       * disabled injection for every git child. */
      /* Only when a cwd IS set. run_cmd_get_cwd() returns NULL outside a bound
       * turn, and those are aimee's own housekeeping git calls — a branch name,
       * a rev-parse — which need no credential and have no workspace to own
       * them. Warning on them fired several times per push with an empty path
       * and said nothing; that was noise this instrumentation introduced. The
       * real anomaly, and all this now reports, is a cwd that exists and that
       * no registered workspace claims. */
      if (have_ws != 0 && cwd && cwd[0])
         LOG_WARN("git",
                  "no registered workspace owns cwd \"%s\": git will run with no forge credential",
                  cwd);
      if (have_ws == 0)
      {
         /* Resolve the git credential through the ONE policy
          * (git_cred_inject_build_env_for_repo) so the precedence never drifts
          * from the other call sites: per-host server vault → the principal's
          * vaulted forge token → server identity → ambient.
          *
          * NO WORKSPACE BROKER TOKEN HERE, deliberately. It used to be fetched
          * and passed as preferred_token, which outranks the vault — so this
          * exec path authenticated as a different, weaker credential than the
          * in-process forge calls next door, which pass none and get the vault.
          * The result was a split personality against the same repository:
          * `pr create` succeeded on the vaulted credential while `push` was
          * refused with "Permission to <repo> denied to <user>", which reads as
          * a permissions problem on an account that in fact has admin. Operator
          * ruling: aimee git proxies through aimee's OWN vaulted credential, so
          * that is the only thing this path may use. */
         int token_fd = -1;
         /* Hand the policy the workspace's RECORDED REMOTE, not just the cwd.
          * The per-host vault step keys on the remote's host, which it derives
          * from an explicit remote URL or, failing that, by running
          * `git -C <repo_dir> config --get remote.origin.url`. For a mirror the
          * cwd we were handed is the CLIENT's path: it does not exist on this
          * server, so that lookup finds nothing, resolution falls through to no
          * token, and git prompts — "could not read Username", the very failure
          * this file's header describes. workspace_turn.c already passes the
          * remote for exactly this reason ("remote URL (per-host vault lookup),
          * so ctx carries both"); this call site is the one that did not.
          * cwd is still passed as the fallback for a shared workspace, where it
          * IS the real checkout and no remote may be recorded. */
         const char *ws_remote = workspace_remote_for_root(wsid);
         char **envp =
             git_cred_inject_build_env_for_repo(NULL, ws_remote, cwd, NULL, environ, &token_fd);
         /* SAY SO WHEN NOTHING WAS STAGED. Falling through to ambient credentials
          * is deliberate for a co-located dev with their own gh/SSH, but a server
          * has none, so the only symptom is git prompting: "could not read
          * Username", which reads as a dead token and sends the reader to the
          * vault. Naming the workspace and the remote resolution keyed on
          * separates "no remote recorded for this workspace" from "the remote is
          * right and the vault has no entry for its host". */
         if (token_fd < 0)
            LOG_WARN("git",
                     "no forge credential staged for workspace \"%s\" (remote=%s): git will run "
                     "unauthenticated and a push will fail asking for a username",
                     wsid, (ws_remote && ws_remote[0]) ? ws_remote : "<none recorded>");
         if (envp)
         {
            /* FD mode: the token rides an inherited memfd, never the environ. */
            char *out = run_cmd_env_fd(cmd, envp, exit_code, token_fd,
                                       token_fd >= 0 ? GIT_CRED_TOKEN_TARGET_FD : -1);
            git_cred_inject_free_env(envp);
            if (token_fd >= 0)
               close(token_fd);
            return out;
         }
         if (token_fd >= 0)
            close(token_fd);
      }
   }
   return exec_ws->exec_shell(exec_ws, cmd, exit_code);
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

/* Check if the given branch is still at the head of its latest merged PR.
 * Returns 1 only when reusing the branch would mutate an already-merged line
 * with no new commits. Long-lived integration branches (for example testing)
 * may accumulate new commits and open another promotion PR. Skips the check
 * for main/master. */
int check_branch_has_merged_pr_for(const char *branch)
{
   if (!branch || !branch[0])
      return 0;
   if (strcmp(branch, "main") == 0 || strcmp(branch, "master") == 0)
      return 0;

   char cmd[512];
   snprintf(cmd, sizeof(cmd),
            "gh pr list --head '%s' --state merged --json number,headRefOid --limit 1 2>/dev/null",
            branch);

   int rc;
   char *out = mcp_git_run(cmd, &rc);
   if (rc != 0 || !out)
   {
      free(out);
      return 0; /* gh not available or failed -- don't block */
   }

   cJSON *prs = cJSON_Parse(out);
   free(out);
   if (!prs || !cJSON_IsArray(prs) || cJSON_GetArraySize(prs) == 0)
   {
      cJSON_Delete(prs);
      return 0;
   }

   cJSON *latest = cJSON_GetArrayItem(prs, 0);
   cJSON *merged_head = cJSON_GetObjectItemCaseSensitive(latest, "headRefOid");
   if (!cJSON_IsString(merged_head) || !merged_head->valuestring[0])
   {
      cJSON_Delete(prs);
      return 1; /* A merged PR exists but its head is unavailable: fail closed. */
   }

   char merged_head_oid[128];
   snprintf(merged_head_oid, sizeof(merged_head_oid), "%s", merged_head->valuestring);
   cJSON_Delete(prs);

   char *current_head = mcp_git_run("git rev-parse HEAD 2>/dev/null", &rc);
   if (rc != 0 || !current_head)
   {
      free(current_head);
      return 1;
   }
   char *nl = strchr(current_head, '\n');
   if (nl)
      *nl = '\0';
   int unchanged_since_merge = (strcmp(current_head, merged_head_oid) == 0);
   free(current_head);
   return unchanged_since_merge;
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
   cJSON *jpath = args ? cJSON_GetObjectItemCaseSensitive(args, "path") : NULL;
   int explicit_path = cJSON_IsString(jpath) && jpath->valuestring[0];
   int no_session_redirect =
       explicit_path ||
       (args && cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(args, "no_session_redirect")));

   /* An explicit path is an authoritative repository identity, not merely the
    * first guess before stale session state. If it cannot be read through the
    * active workspace provider, fail closed instead of falling through to a
    * different checkout. Clone dispatch removes its destination path before
    * calling this resolver because that path need not exist yet. */
   if (explicit_path)
      mcp_git_add_candidate(candidates, &candidate_count, 8, jpath->valuestring);

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
   if (!explicit_path && args)
   {
      cJSON *jcwd = cJSON_GetObjectItemCaseSensitive(args, "cwd");
      if (cJSON_IsString(jcwd) && jcwd->valuestring[0])
         mcp_git_add_candidate(candidates, &candidate_count, 8, jcwd->valuestring);
   }

   /* Priority 4: environment fallbacks. Some MCP hosts launch the stdio
    * wrapper from / but preserve a meaningful PWD or explicit override. */
   if (!explicit_path)
   {
      mcp_git_add_candidate(candidates, &candidate_count, 8, getenv("AIMEE_MCP_CWD"));
      mcp_git_add_candidate(candidates, &candidate_count, 8, getenv("PWD"));
   }

   /* Priority 5: process CWD (racy in multi-session but useful as a fallback) */
   if (!explicit_path)
   {
      char cwd_buf[MAX_PATH_LEN];
      if (getcwd(cwd_buf, sizeof(cwd_buf)))
         mcp_git_add_candidate(candidates, &candidate_count, 8, cwd_buf);
   }

   /* Priority 6: executable directory. This catches repo-local plugin/server
    * launches where the process cwd is not the checkout root. */
   if (!explicit_path)
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
      return explicit_path ? -2 : 0; /* Explicit identity never falls through. */

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
