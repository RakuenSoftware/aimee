/* git_ops.c — per-project git operations for webchat users. See git_ops.h. */
#include <aimee/git/git_ops.h>
#include "git_cred_inject.h"                   /* git_cred_inject_build_env / _free_env */
#include "git_pr_api.h"                        /* git_pr_create_via_api — in-process REST open-PR */
#include "util.h"                              /* safe_exec_capture_cwd_env_timeout */
#include "modules/workspace/workspace_scope.h" /* ws_scope_project_path */

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

static git_ops_ref_validator_fn g_ref_validator;

void git_ops_register_ref_validator(git_ops_ref_validator_fn validator)
{
   g_ref_validator = validator;
}

static int ref_allowed(const char *ref)
{
   int allowed = 0;
   return g_ref_validator && g_ref_validator(ref, &allowed) == 0 && allowed;
}

static int validate_managed_ref(const char *ref, char *err, size_t errlen)
{
   if (!ref || !ref[0])
   {
      snprintf(err, errlen, "managed push ref is required");
      return -1;
   }
   if (!g_ref_validator)
   {
      snprintf(err, errlen, "managed push ref validator is unavailable");
      return -1;
   }
   int allowed = 0;
   int result = g_ref_validator(ref, &allowed);
   if (result != 0)
   {
      snprintf(err, errlen, "managed push ref validation failed (result=%d)", result);
      return -1;
   }
   if (!allowed)
   {
      snprintf(err, errlen, "managed push ref is invalid");
      return -1;
   }
   return 0;
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

static int env_key_is(const char *entry, const char *key)
{
   size_t n = strlen(key);
   return strncmp(entry, key, n) == 0 && entry[n] == '=';
}

/* Remove every environment-driven git-config injection seam and pin system +
 * global config off. Repository config remains available only for ordinary
 * object/ref data; the managed push argv explicitly fixes hooks, credentials,
 * protocol, destination, and refspec. */
static char **harden_push_env(char **source)
{
   /* Deliberately allowlist only the fd-fed askpass seam. Do not inherit HOME,
    * GIT_DIR/WORK_TREE/object/index selectors, proxy/TLS/header overrides, or
    * any other repository/transport control from the daemon environment. */
   char **out = calloc(8, sizeof(char *));
   if (!out)
      return NULL;
   size_t n = 0;
   for (size_t i = 0; source && source[i]; i++)
   {
      if (!env_key_is(source[i], "GIT_ASKPASS") && !env_key_is(source[i], "GIT_TERMINAL_PROMPT") &&
          !env_key_is(source[i], "AIMEE_GIT_TOKEN_FD"))
         continue;
      out[n] = strdup(source[i]);
      if (!out[n++])
         goto fail;
   }
   out[n++] = strdup("PATH=/usr/local/bin:/usr/bin:/bin");
   out[n++] = strdup("GIT_CONFIG_NOSYSTEM=1");
   out[n++] = strdup("GIT_CONFIG_SYSTEM=/dev/null");
   out[n++] = strdup("GIT_CONFIG_GLOBAL=/dev/null");
   if (!out[n - 1] || !out[n - 2] || !out[n - 3] || !out[n - 4])
      goto fail;
   return out;
fail:
   git_cred_inject_free_env(out);
   return NULL;
}

int git_ops_push_dir(const char *principal, const char *repo_dir, const char *remote_url,
                     const char *branch, char **out, char *err, size_t errlen)
{
   if (out)
      *out = NULL;
   if (err && errlen)
      err[0] = '\0';
   if (!repo_dir || !repo_dir[0] || !remote_url || !remote_url[0])
   {
      snprintf(err, errlen, "managed push repository and remote are required");
      return -1;
   }
   if (validate_managed_ref(branch, err, errlen) != 0)
      return -1;
   char refspec[420];
   if ((size_t)snprintf(refspec, sizeof(refspec), "%s:%s", branch, branch) >= sizeof(refspec))
   {
      snprintf(err, errlen, "managed push ref too long");
      return -1;
   }
   const char *argv[] = {"git",
                         "-c",
                         "core.hooksPath=/dev/null",
                         "-c",
                         "credential.helper=",
                         "-c",
                         "protocol.allow=never",
                         "-c",
                         "protocol.https.allow=always",
                         "push",
                         "-u",
                         remote_url,
                         refspec,
                         NULL};
   int token_fd = -1;
   char **envp =
       git_cred_inject_build_env_for_repo(principal, remote_url, NULL, NULL, environ, &token_fd);
   char **hardened = harden_push_env(envp ? envp : environ);
   if (!hardened)
   {
      if (token_fd >= 0)
         close(token_fd);
      if (envp)
         git_cred_inject_free_env(envp);
      snprintf(err, errlen, "cannot construct hardened git environment");
      return -1;
   }

   /* Repository-local config is data controlled by the checkout. Reject every
    * setting capable of rewriting the explicit URL, injecting auth/headers,
    * changing TLS/proxy behavior, or selecting an external transport. rc=1 is
    * git-config's clean 'no matches'; every other nonzero result fails closed. */
   const char *config_argv[] = {
       "git",
       "-C",
       repo_dir,
       "config",
       "--local",
       "--includes",
       "--name-only",
       "--get-regexp",
       "^(url\\..*\\.insteadof|http\\.|credential\\.|core\\.sshcommand|remote\\..*\\.(proxy|vcs))",
       NULL};
   char *unsafe_config = NULL;
   int config_rc = safe_exec_capture_cwd_env_fd_timeout(config_argv, repo_dir, hardened,
                                                        &unsafe_config, 65536, 5000, -1, -1);
   if (config_rc != 1 || (unsafe_config && unsafe_config[0]))
   {
      free(unsafe_config);
      if (token_fd >= 0)
         close(token_fd);
      if (envp)
         git_cred_inject_free_env(envp);
      git_cred_inject_free_env(hardened);
      snprintf(err, errlen, "unsafe or unreadable repository-local git configuration");
      return -1;
   }
   free(unsafe_config);
   int rc = safe_exec_capture_cwd_env_fd_timeout(argv, repo_dir, hardened, out, GO_OUT_MAX,
                                                 GO_TIMEOUT_MS, token_fd,
                                                 token_fd >= 0 ? GIT_CRED_TOKEN_TARGET_FD : -1);
   if (token_fd >= 0)
      close(token_fd);
   if (envp)
      git_cred_inject_free_env(envp);
   if (hardened)
      git_cred_inject_free_env(hardened);
   if (rc != 0)
   {
      /* Preserve the complete capture in *out. The typed resource route carries
       * it in a separate detail field, so diagnostics are never byte-truncated
       * into this fixed-size summary buffer. */
      snprintf(err, errlen, "git push failed (rc=%d)", rc);
      return -1;
   }
   return 0;
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

static git_ops_classifier_fn g_classifier;

void git_ops_register_classifier(git_ops_classifier_fn classifier)
{
   g_classifier = classifier;
}

static int classify_operation(const char *op, aimee_git_classification_t *out)
{
   if (!op || !out)
      return -1;
   if (g_classifier)
      return g_classifier(op, out);
   memset(out, 0, sizeof(*out));
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

   aimee_git_classification_t classification;
   if (classify_operation(op, &classification) != 0 ||
       classification.operation == AIMEE_GIT_OP_UNSUPPORTED)
   {
      snprintf(err, errlen, "unsupported op");
      return -1;
   }

   char dir[GO_PATH_MAX];
   if (resolve_session_dir(principal, project, session_id, dir, sizeof(dir), err, errlen) != 0)
      return -1;

   /* --- commit is two steps (stage all, then commit with the message) --- */
   if (classification.operation == AIMEE_GIT_OP_COMMIT)
   {
      if (!text_arg || !text_arg[0] || strlen(text_arg) > 4000)
      {
         snprintf(err, errlen, "commit requires a non-empty message");
         return -1;
      }
      /* Standing directive: no AI co-authorship trailers / "Generated with"
       * attribution in commits — strip before the message reaches git. */
      char msg[4096];
      snprintf(msg, sizeof(msg), "%s", text_arg);
      strip_ai_attribution(msg);
      if (!msg[0])
      {
         snprintf(err, errlen, "commit message was only AI attribution lines");
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
      const char *argv[] = {"git", "commit", "-m", msg, NULL};
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
   if (classification.operation == AIMEE_GIT_OP_PR)
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
   int needs_cred = classification.needs_credentials;
   char nbuf[16];

   if (classification.operation == AIMEE_GIT_OP_STATUS)
   {
      argv[0] = "git";
      argv[1] = "status";
      argv[2] = "--porcelain=v1";
      argv[3] = "-b";
   }
   else if (classification.operation == AIMEE_GIT_OP_LOG)
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
   else if (classification.operation == AIMEE_GIT_OP_DIFF)
   {
      argv[0] = "git";
      argv[1] = "diff";
   }
   else if (classification.operation == AIMEE_GIT_OP_BRANCH)
   {
      argv[0] = "git";
      argv[1] = "branch";
      argv[2] = "--list";
      argv[3] = "--no-color";
   }
   else if (classification.operation == AIMEE_GIT_OP_FETCH)
   {
      argv[0] = "git";
      argv[1] = "fetch";
      argv[2] = "--prune";
   }
   else if (classification.operation == AIMEE_GIT_OP_PULL)
   {
      argv[0] = "git";
      argv[1] = "pull";
      argv[2] = "--ff-only";
   }
   else if (classification.operation == AIMEE_GIT_OP_PUSH)
   {
      argv[0] = "git";
      argv[1] = "push";
   }
   else if (classification.operation == AIMEE_GIT_OP_CHECKOUT)
   {
      if (!ref_allowed(text_arg))
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
