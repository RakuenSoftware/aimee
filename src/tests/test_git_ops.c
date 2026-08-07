/* test_git_ops.c — WP-E: per-project git operations, scoped + sanitized.
 * Builds a real repo (+ a local bare remote) under a webuser's scope and drives
 * status/log/branch/diff/commit/checkout/push/pull, plus the refusal paths. */
#include <aimee/git/git_ops.h>
#include <aimee/core/event_bus/module_runtime.h>
#include <aimee/workspace/module_api.h>

#include "modules/workspace/workspace_scope.h"

#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern aimee_module_status_t aimee_workspace_module_handler(const aimee_module_invocation_t *,
                                                            const uint8_t *, uint32_t, uint8_t *,
                                                            uint32_t, uint32_t *, void *);

int aimee_module_invocation_cancelled(const aimee_module_invocation_t *invocation)
{
   (void)invocation;
   return 0;
}

static int validate_workspace_ref_via_module(const char *ref, size_t ref_len, int *allowed)
{
   uint8_t request[AIMEE_WORKSPACE_REQUEST_LEN], response[AIMEE_WORKSPACE_RESPONSE_LEN];
   uint32_t response_len = 0;
   aimee_module_invocation_t invocation = {.stage_id = AIMEE_WORKSPACE_STAGE_ACCESS};
   return aimee_workspace_request_encode(ref, ref_len, request, sizeof(request)) == 0 &&
                  aimee_workspace_module_handler(&invocation, request, sizeof(request), response,
                                                 sizeof(response), &response_len,
                                                 NULL) == AIMEE_MODULE_STATUS_OK
              ? aimee_workspace_response_decode(response, response_len, allowed)
              : -1;
}

static int run(const char *fmt, ...)
{
   char cmd[2048];
   va_list ap;
   va_start(ap, fmt);
   vsnprintf(cmd, sizeof(cmd), fmt, ap);
   va_end(ap);
   return system(cmd);
}

static int out_has(const char *out, const char *needle)
{
   return out && strstr(out, needle) != NULL;
}

/* Fake per-session worktree resolver: maps <cwd> -> <cwd>/wt-<sid>. */
static int fake_isolation(const char *cwd, const char *sid, char *out, size_t out_len, int create)
{
   (void)create;
   if (!cwd || !sid || !sid[0])
      return 0;
   snprintf(out, out_len, "%s/wt-%s", cwd, sid);
   return 1;
}

/* The production classifier is an event-bus module. This integration test
 * isolates filesystem/git execution, so provide the module's fixed contract
 * locally; process-handler vectors cover the actual module implementation. */
static int fake_classifier(const char *op, aimee_git_classification_t *out)
{
   static const struct
   {
      const char *name;
      aimee_git_operation_t operation;
      int needs_credentials;
   } rows[] = {{"status", AIMEE_GIT_OP_STATUS, 0}, {"log", AIMEE_GIT_OP_LOG, 0},
               {"diff", AIMEE_GIT_OP_DIFF, 0},     {"branch", AIMEE_GIT_OP_BRANCH, 0},
               {"fetch", AIMEE_GIT_OP_FETCH, 1},   {"pull", AIMEE_GIT_OP_PULL, 1},
               {"push", AIMEE_GIT_OP_PUSH, 1},     {"checkout", AIMEE_GIT_OP_CHECKOUT, 0},
               {"commit", AIMEE_GIT_OP_COMMIT, 0}, {"pr", AIMEE_GIT_OP_PR, 1}};
   if (!op || !out)
      return -1;
   memset(out, 0, sizeof(*out));
   for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); ++i)
   {
      if (strcmp(op, rows[i].name) != 0)
         continue;
      out->operation = rows[i].operation;
      out->needs_credentials = rows[i].needs_credentials;
      break;
   }
   return 0;
}

static int fake_ref_validator(const char *ref, int *allowed)
{
   if (!ref || !allowed)
      return -1;
   if (strcmp(ref, "validator-error") == 0)
      return -1;
   *allowed = strcmp(ref, "-evil") != 0;
   return 0;
}

int main(void)
{
   ws_scope_register_ref_validator(validate_workspace_ref_via_module);
   git_ops_register_classifier(fake_classifier);
   git_ops_register_ref_validator(fake_ref_validator);
   char home[256];
   snprintf(home, sizeof(home), "/tmp/aimee-gitops-%d", (int)getpid());
   assert(run("rm -rf %s && mkdir -p %s", home, home) == 0);
   setenv("AIMEE_HOME", home, 1);
   char ws[300];
   snprintf(ws, sizeof(ws), "%s/ws", home);
   setenv("AIMEE_WORKSPACES_DIR", ws, 1);
   setenv("GIT_CONFIG_COUNT", "1", 1);
   setenv("GIT_CONFIG_KEY_0", "protocol.file.allow", 1);
   setenv("GIT_CONFIG_VALUE_0", "always", 1);

   /* alice's project dir + a bare remote it tracks. */
   char proj[400], bare[400];
   snprintf(proj, sizeof(proj), "%s/environment/proj", ws);
   snprintf(bare, sizeof(bare), "%s/remote.git", home);
   assert(run("git init -q --bare %s", bare) == 0);
   assert(run("mkdir -p %s && cd %s && git init -q -b main && git config user.email t@t && git "
              "config user.name t && echo one > f.txt && git add . && git commit -qm init && git "
              "remote add origin file://%s && git push -qu origin main",
              proj, proj, bare) == 0);

   char *out = NULL;
   char err[256];

   /* status */
   assert(git_ops_run("webuser:alice", "proj", "status", NULL, 0, &out, err, sizeof(err)) == 0);
   assert(out_has(out, "main"));
   free(out);

   /* log shows the init commit */
   assert(git_ops_run("webuser:alice", "proj", "log", NULL, 5, &out, err, sizeof(err)) == 0);
   assert(out_has(out, "init"));
   free(out);

   /* branch lists main */
   assert(git_ops_run("webuser:alice", "proj", "branch", NULL, 0, &out, err, sizeof(err)) == 0);
   assert(out_has(out, "main"));
   free(out);

   /* commit a change, then log shows it */
   assert(run("cd %s && echo two >> f.txt", proj) == 0);
   assert(git_ops_run("webuser:alice", "proj", "commit", "second change", 0, &out, err,
                      sizeof(err)) == 0);
   free(out);
   assert(git_ops_run("webuser:alice", "proj", "log", NULL, 5, &out, err, sizeof(err)) == 0);
   assert(out_has(out, "second change"));
   free(out);

   /* diff after staging is empty; create an unstaged change and diff sees it */
   assert(run("cd %s && echo three >> f.txt", proj) == 0);
   assert(git_ops_run("webuser:alice", "proj", "diff", NULL, 0, &out, err, sizeof(err)) == 0);
   assert(out_has(out, "three"));
   free(out);

   /* push the committed change to the bare remote */
   assert(git_ops_run("webuser:alice", "proj", "push", NULL, 0, &out, err, sizeof(err)) == 0);
   free(out);
   /* fetch + pull are no-ops but must succeed */
   assert(git_ops_run("webuser:alice", "proj", "fetch", NULL, 0, &out, err, sizeof(err)) == 0);
   free(out);
   assert(git_ops_run("webuser:alice", "proj", "pull", NULL, 0, &out, err, sizeof(err)) == 0);
   free(out);

   /* Managed WFE pushes use deeper refs than ordinary feature branches. Exercise the exact
    * production shape at the git-execution seam, and keep capability absence, capability
    * failure, and a policy denial distinguishable for operators. The valid ref reaches git;
    * localhost port 1 then refuses immediately, proving validation did not stop it. */
   const char *wfe_ref = "aimee/wi/wi_57186250728b511961573e5afb37cc93.s4263a4834d.g0.0";
   git_ops_register_ref_validator(NULL);
   assert(git_ops_push_dir("webuser:alice", proj, "https://127.0.0.1:1/probe.git", wfe_ref, &out,
                           err, sizeof(err)) == -1);
   assert(strstr(err, "validator is unavailable") != NULL);
   git_ops_register_ref_validator(fake_ref_validator);
   assert(git_ops_push_dir("webuser:alice", proj, "https://127.0.0.1:1/probe.git", wfe_ref, &out,
                           err, sizeof(err)) == -1);
   assert(strstr(err, "git push failed") != NULL);
   free(out);
   out = NULL;
   assert(git_ops_push_dir("webuser:alice", proj, "https://127.0.0.1:1/probe.git", "-evil", &out,
                           err, sizeof(err)) == -1);
   assert(strstr(err, "ref is invalid") != NULL);
   assert(git_ops_push_dir("webuser:alice", proj, "https://127.0.0.1:1/probe.git",
                           "validator-error", &out, err, sizeof(err)) == -1);
   assert(strstr(err, "validation failed (result=-1)") != NULL);

   /* checkout an existing branch */
   assert(run("cd %s && git branch -q feature", proj) == 0);
   git_ops_register_ref_validator(NULL);
   assert(git_ops_run("webuser:alice", "proj", "checkout", "feature", 0, &out, err, sizeof(err)) ==
          -1);
   assert(strstr(err, "invalid branch name") != NULL);
   git_ops_register_ref_validator(fake_ref_validator);
   assert(git_ops_run("webuser:alice", "proj", "checkout", "feature", 0, &out, err, sizeof(err)) ==
          0);
   free(out);

   /* --- refusals --- */
   assert(git_ops_run("uid:1000", "proj", "status", NULL, 0, &out, err, sizeof(err)) == -1);
   assert(git_ops_run("webuser:alice", "nope", "status", NULL, 0, &out, err, sizeof(err)) == -1);
   assert(git_ops_run("webuser:alice", "proj", "rm -rf /", NULL, 0, &out, err, sizeof(err)) == -1);
   assert(git_ops_run("webuser:alice", "proj", "checkout", "-evil", 0, &out, err, sizeof(err)) ==
          -1);
   assert(git_ops_run("webuser:alice", "proj", "commit", "", 0, &out, err, sizeof(err)) == -1);
   assert(git_ops_run("webuser:alice", "../escape", "status", NULL, 0, &out, err, sizeof(err)) ==
          -1);
   /* Another actor reaches the same project: one environment, and PAM identity
    * authorizes and attributes the request rather than selecting a tree. */
   assert(git_ops_run("webuser:bob", "proj", "status", NULL, 0, &out, err, sizeof(err)) == 0);
   free(out);
   out = NULL;

   /* --- session worktree redirect (git_ops_session_dir wiring) --- */
   char dir[4096];
   /* no session id -> the project checkout */
   assert(git_ops_session_dir("webuser:alice", "proj", NULL, dir, sizeof(dir), err, sizeof(err)) ==
          0);
   assert(strcmp(dir, proj) == 0);
   /* session id but no resolver registered -> still the project checkout */
   assert(git_ops_session_dir("webuser:alice", "proj", "sid-xyz", dir, sizeof(dir), err,
                              sizeof(err)) == 0);
   assert(strcmp(dir, proj) == 0);
   /* with a resolver: a non-empty session id redirects to its worktree */
   git_ops_register_session_isolation(fake_isolation);
   assert(git_ops_session_dir("webuser:alice", "proj", "sid-xyz", dir, sizeof(dir), err,
                              sizeof(err)) == 0);
   char expect[4200];
   snprintf(expect, sizeof(expect), "%s/wt-sid-xyz", proj);
   assert(strcmp(dir, expect) == 0);
   /* empty session id -> base even with a resolver registered */
   assert(git_ops_session_dir("webuser:alice", "proj", "", dir, sizeof(dir), err, sizeof(err)) ==
          0);
   assert(strcmp(dir, proj) == 0);
   /* refusal: a non-webuser principal is rejected before any resolution */
   assert(git_ops_session_dir("uid:1000", "proj", "sid-xyz", dir, sizeof(dir), err, sizeof(err)) ==
          -1);
   git_ops_register_session_isolation(NULL);

   assert(run("rm -rf %s", home) == 0);
   printf("git_ops: all tests passed\n");
   return 0;
}
