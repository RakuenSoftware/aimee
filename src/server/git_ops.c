/* git_ops.c — per-project git operations for webchat users. See git_ops.h. */
#include "git_ops.h"
#include "git_cred_inject.h" /* git_cred_inject_build_env / _free_env */
#include "git_pr_api.h"      /* git_pr_create_via_api — in-process REST open-PR */
#include "util.h"            /* safe_exec_capture_cwd_env_timeout */
#include "workspace_scope.h" /* ws_scope_project_path */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h> /* close — release the token memfd after the git exec */

extern char **environ;

/* Per-session worktree resolver (workspace.c:session_isolation_target), wired in
 * by the server at startup via a registered pointer so this TU carries no link
 * dependency on the heavyweight workspace.o. Unregistered (thin client / unit
 * tests) → a session op simply runs in the shared project checkout. */
static int (*g_session_isolation_target)(const char *cwd, const char *sid, char *out,
                                         size_t out_len, int create_if_missing);

void git_ops_register_session_isolation(int (*fn)(const char *cwd, const char *sid, char *out,
                                                  size_t out_len, int create_if_missing))
{
   g_session_isolation_target = fn;
}

#define GO_PATH_MAX    4096
#define GO_OUT_MAX     (1 << 18) /* 256 KiB of git output */
#define GO_TIMEOUT_MS  120000    /* a git op (incl. network) may take a while */
#define GO_LOG_DEFAULT 30
#define GO_LOG_MAX     200

/* A git ref/branch name safe to pass as an argv token: non-empty, <=200, no
 * control chars/space, not starting with '-' (flag), no "..", charset limited to
 * [A-Za-z0-9._/-]. (git imposes more rules; this is a conservative subset.) */
static int ref_name_valid(const char *s)
{
   if (!s || !s[0] || s[0] == '-')
      return 0;
   size_t n = strlen(s);
   if (n > 200)
      return 0;
   if (strstr(s, ".."))
      return 0;
   for (size_t i = 0; i < n; i++)
   {
      unsigned char c = (unsigned char)s[i];
      int ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
               c == '.' || c == '_' || c == '/' || c == '-';
      if (!ok)
         return 0;
   }
   return 1;
}

/* Run argv in `dir` with creds injected when `needs_cred`. Returns the child
 * exit code (0 = ok), -1 on fork/pipe failure; *out receives the captured
 * output (caller frees). */
static int run_git(const char *principal, const char *dir, const char *const argv[], int needs_cred,
                   char **out)
{
   /* Resolve credentials vault-first for THIS repo: the per-host vault token for
    * the checkout's `origin` host wins over the principal/server identity, so a
    * push/fetch to gitlab/gitea authenticates with the right host's stored token
    * (not the server's GitHub identity). repo_dir = dir → origin is resolved only
    * when a host token is actually needed (fetch/pull/push). */
   int token_fd = -1;
   char **envp = needs_cred ? git_cred_inject_build_env_for_repo(principal, NULL, dir, NULL,
                                                                 environ, &token_fd)
                            : NULL;
   /* FD mode: the HTTPS token rides an inherited memfd (token_fd), placed at
    * GIT_CRED_TOKEN_TARGET_FD in the git child where the askpass reads it — so it
    * never lands in the child's /proc/<pid>/environ. The fd is CLOEXEC here, so a
    * concurrent exec on another thread can't inherit it. */
   int rc = safe_exec_capture_cwd_env_fd_timeout(argv, dir, envp ? envp : environ, out, GO_OUT_MAX,
                                                 GO_TIMEOUT_MS, token_fd,
                                                 token_fd >= 0 ? GIT_CRED_TOKEN_TARGET_FD : -1);
   if (token_fd >= 0)
      close(token_fd);
   if (envp)
      git_cred_inject_free_env(envp);
   return rc;
}

/* Resolve the working dir for `project`: the project checkout, or — when
 * `session_id` is non-empty and a session-isolation resolver is registered — that
 * session's sibling worktree (off the default branch, created on demand) so the
 * webchat git surfaces act on the SAME tree the session's agent edits. Returns 0
 * + dir, or -1 with err. */
static int resolve_session_dir(const char *principal, const char *project, const char *session_id,
                               char *dir, size_t dir_len, char *err, size_t errlen)
{
   if (ws_scope_project_path(principal, project ? project : "", 1 /*must_exist*/, dir, dir_len) !=
       0)
   {
      snprintf(err, errlen, "no such project");
      return -1;
   }
   if (session_id && session_id[0] && g_session_isolation_target)
   {
      char wt[GO_PATH_MAX];
      if (g_session_isolation_target(dir, session_id, wt, sizeof(wt), 1 /*create_if_missing*/) == 1)
         snprintf(dir, dir_len, "%s", wt);
   }
   return 0;
}

int git_ops_session_dir(const char *principal, const char *project, const char *session_id,
                        char *out, size_t out_len, char *err, size_t errlen)
{
   if (out && out_len)
      out[0] = '\0';
   if (err && errlen)
      err[0] = '\0';
   if (!principal || strncmp(principal, "webuser:", 8) != 0)
   {
      snprintf(err, errlen, "git projects require a webchat user");
      return -1;
   }
   char dir[GO_PATH_MAX];
   if (resolve_session_dir(principal, project, session_id, dir, sizeof(dir), err, errlen) != 0)
      return -1;
   snprintf(out, out_len, "%s", dir);
   return 0;
}

int git_ops_run(const char *principal, const char *project, const char *op, const char *text_arg,
                int num_arg, char **out, char *err, size_t errlen)
{
   return git_ops_run_session(principal, project, NULL, op, text_arg, num_arg, out, err, errlen);
}

int git_ops_run_session(const char *principal, const char *project, const char *session_id,
                        const char *op, const char *text_arg, int num_arg, char **out, char *err,
                        size_t errlen)
{
   if (out)
      *out = NULL;
   if (err && errlen)
      err[0] = '\0';

   if (!principal || strncmp(principal, "webuser:", 8) != 0)
   {
      snprintf(err, errlen, "git projects require a webchat user");
      return -1;
   }
   if (!op || !op[0])
   {
      snprintf(err, errlen, "missing op");
      return -1;
   }

   char dir[GO_PATH_MAX];
   if (resolve_session_dir(principal, project, session_id, dir, sizeof(dir), err, errlen) != 0)
      return -1;

   /* --- commit is two steps (stage all, then commit with the message) --- */
   if (strcmp(op, "commit") == 0)
   {
      if (!text_arg || !text_arg[0] || strlen(text_arg) > 4000)
      {
         snprintf(err, errlen, "commit requires a non-empty message");
         return -1;
      }
      const char *add_argv[] = {"git", "add", "-A", NULL};
      char *add_out = NULL;
      int arc = run_git(principal, dir, add_argv, 0, &add_out);
      free(add_out);
      if (arc != 0)
      {
         snprintf(err, errlen, "git add failed (rc=%d)", arc);
         return -1;
      }
      const char *argv[] = {"git", "commit", "-m", text_arg, NULL};
      int rc = run_git(principal, dir, argv, 0, out);
      if (rc != 0)
      {
         snprintf(err, errlen, "git commit failed (rc=%d)%s%.180s", rc,
                  (out && *out && (*out)[0]) ? ": " : "", (out && *out) ? *out : "");
         free(*out);
         *out = NULL;
         return -1;
      }
      return 0;
   }

   /* --- open-PR is an in-process GitHub REST call (git_pr_api), NOT a child
    * exec: the forge token rides the Authorization header in aimee-server memory
    * and never reaches a child's environ/argv (gh would put it in GH_TOKEN). The
    * title (text_arg) is optional — empty defaults to the last commit subject.
    * Like every git_ops op this is the webuser acting on their OWN connected repo
    * (no agent branch-ownership/verify gate), confined by the principal-scoped
    * project resolution + route caps + AIMEE_WEBCHAT_GIT. GitHub origins only. */
   if (strcmp(op, "pr") == 0)
   {
      if (text_arg && strlen(text_arg) > 256)
      {
         snprintf(err, errlen, "pr title too long");
         return -1;
      }
      char url[1024];
      if (git_pr_create_via_api(principal, dir, text_arg, NULL, url, sizeof(url), err, errlen) != 0)
         return -1;
      if (out)
         *out = strdup(url);
      return 0;
   }

   /* --- single-command ops: build argv + cred requirement --- */
   const char *argv[8] = {0};
   int needs_cred = 0;
   char nbuf[16];

   if (strcmp(op, "status") == 0)
   {
      argv[0] = "git";
      argv[1] = "status";
      argv[2] = "--porcelain=v1";
      argv[3] = "-b";
   }
   else if (strcmp(op, "log") == 0)
   {
      int n = (num_arg > 0) ? num_arg : GO_LOG_DEFAULT;
      if (n > GO_LOG_MAX)
         n = GO_LOG_MAX;
      snprintf(nbuf, sizeof(nbuf), "%d", n);
      argv[0] = "git";
      argv[1] = "log";
      argv[2] = "--oneline";
      argv[3] = "-n";
      argv[4] = nbuf;
   }
   else if (strcmp(op, "diff") == 0)
   {
      argv[0] = "git";
      argv[1] = "diff";
   }
   else if (strcmp(op, "branch") == 0)
   {
      argv[0] = "git";
      argv[1] = "branch";
      argv[2] = "--list";
      argv[3] = "--no-color";
   }
   else if (strcmp(op, "fetch") == 0)
   {
      argv[0] = "git";
      argv[1] = "fetch";
      argv[2] = "--prune";
      needs_cred = 1;
   }
   else if (strcmp(op, "pull") == 0)
   {
      argv[0] = "git";
      argv[1] = "pull";
      argv[2] = "--ff-only";
      needs_cred = 1;
   }
   else if (strcmp(op, "push") == 0)
   {
      argv[0] = "git";
      argv[1] = "push";
      needs_cred = 1;
   }
   else if (strcmp(op, "checkout") == 0)
   {
      if (!ref_name_valid(text_arg))
      {
         snprintf(err, errlen, "invalid branch name");
         return -1;
      }
      argv[0] = "git";
      argv[1] = "checkout";
      argv[2] = text_arg;
   }
   else
   {
      snprintf(err, errlen, "unsupported op");
      return -1;
   }

   int rc = run_git(principal, dir, argv, needs_cred, out);
   if (rc != 0)
   {
      snprintf(err, errlen, "git %s failed (rc=%d)%s%.180s", op, rc,
               (out && *out && (*out)[0]) ? ": " : "", (out && *out) ? *out : "");
      free(*out);
      *out = NULL;
      return -1;
   }
   return 0;
}
