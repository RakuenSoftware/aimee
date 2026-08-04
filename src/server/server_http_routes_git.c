/* server_http_routes_git.c: the webchat git-surface route handlers (workspace
 * clone/list/delete/git-ops/session-dir/editor + per-host credentials and the
 * webuser ssh key), split out of server_http_routes.c to stay under the
 * line-check ceiling (same precedent as server_http_config_routes.c /
 * server_dev_submit.c). The route TABLE stays in server_http_routes.c; the
 * handlers here have external linkage and are declared in
 * server_http_internal.h. Pure relocation — no behavior changes. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "server_http_internal.h"
#include "server_http_identity.h" /* attested X-Aimee-Webuser principal */
#include "aimee_home.h"           /* aimee_home — managed Go WFE worktree root */
#include "cJSON.h"
#include "config.h" /* MAX_PATH_LEN */
#include "log.h"
#include "modules/git/git_forge_vault.h" /* GIT_FORGE_VAULT_AGENT/SSHKEY_CRED — per-webuser ssh-key vault */
#include "modules/git/git_host_cred.h" /* per-host git credential store for /v1/git/credentials */
#include <aimee/git/git_ops.h>         /* git_ops_run for /v1/workspace/git (WP-E) */
#include "modules/git/git_pr_api.h"    /* narrow in-process forge operations for Go WFE */
#include "modules/git/git_org_repos.h" /* git_org_repos_list for /v1/workspace/org-repos */
#include "modules/git/git_project.h" /* git_project_clone/_delete for /v1/workspace/clone + delete */
#include "modules/git/git_ssh_agent.h" /* git_ssh_agent_stop — drop live key handles on revoke */
#include "index.h"                     /* index_scan_project after a webuser clone (WP-D) */
#include "kb_client.h"      /* kb_client_index_scan — push webuser clones into aimee-kb */
#include "vault_service.h"  /* vault_service_set/delete for the per-webuser ssh-key route */
#include "webuser_editor.h" /* webuser_editor_ensure for /v1/workspace/editor (WP-I) */
#include "modules/workspace/workspace_scope.h" /* ws_scope_user_root — project workspace root */
#include "util.h"     /* bounded argv execution for structural worktree checks */
#include "util_url.h" /* util_url_is_remote — reject file:// / local-path clone urls */
#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

static const char *route_json_string(const cJSON *object, const char *key)
{
   const cJSON *value = object ? cJSON_GetObjectItemCaseSensitive(object, key) : NULL;
   return (cJSON_IsString(value) && value->valuestring) ? value->valuestring : NULL;
}

/* AIMEE_WEBCHAT_GIT=0 disables the whole webchat git surface — forge-token,
 * clone, ops, per-host credentials, ssh-key, projects, session-dir, OAuth —
 * without affecting the editor (which has its own AIMEE_WEBCHAT_EDITOR gate;
 * session-dir is git-panel-only and not on the editor path, so gating it here is
 * safe). On by default; only the exact value "0" disables it, so a blank/other
 * value leaves git enabled. Read per call (immediate single-byte compare, like
 * webuser_editor's AIMEE_WEBCHAT_EDITOR gate) — aimee never setenv()s at request
 * time, so there is no concurrent-mutation window. Each git route checks this
 * first and returns 503 when off — ahead of the 403 webuser check, which is
 * intentional: the disabled state is not a secret, and an operator can ship the
 * image with the editor but no git intake. */
int git_surface_enabled(void)
{
   const char *v = getenv("AIMEE_WEBCHAT_GIT");
   return !(v && v[0] == '0' && v[1] == '\0');
}

/* Push a freshly cloned webuser project into aimee-kb (/v1/code/scan) so the
 * curator queues its code units for synthesis + embedding. index_scan_project
 * only feeds this server's local lexical index — without this push a
 * GUI-cloned repo never reaches the kb (no code_embeddings, no corpus).
 * Best-effort: clone success never depends on the knowledge service; the
 * outcome is reported on `out` as kb_indexed (+ kb_reason on failure). */
static void rh_clone_kb_scan(const char *pname, const char *dest, cJSON *out)
{
   if (!kb_client_is_live())
   {
      cJSON_AddBoolToObject(out, "kb_indexed", 0);
      cJSON_AddStringToObject(out, "kb_reason", "knowledge service unavailable");
      return;
   }
   kb_client_index_scan_result_t res;
   memset(&res, 0, sizeof(res));
   int rc = kb_client_index_scan(pname, dest, 0, &res);
   int ok = (rc == 0 && !res.skipped);
   cJSON_AddBoolToObject(out, "kb_indexed", ok);
   if (!ok)
   {
      cJSON_AddStringToObject(out, "kb_reason",
                              res.reason[0] ? res.reason : "knowledge service unavailable");
      LOG_WARN("server.workspace", "clone: kb code scan skipped for project '%s': %s", pname,
               res.message[0] ? res.message : (res.reason[0] ? res.reason : "unavailable"));
   }
}

/* POST /v1/workspace/clone {url, name?} — clone a repo as a project under the
 * calling webchat user's scoped workspace (webchat-git WP-D). The caller
 * principal comes from the attested identity (root-UDS-gated X-Aimee-Webuser),
 * NOT the body — a user can only clone into their own tree. Credentials are
 * injected from the user's sealed vault (WP-C); never accepted in the body. */
int rh_workspace_clone(const route_req_t *rq, char *resp, int cap)
{
   if (!git_surface_enabled())
      return err_json(resp, cap, 503, "the git surface is disabled on this server");
   const char *principal = server_http_identity_principal();
   if (!principal || strncmp(principal, "webuser:", 8) != 0)
      return err_json(resp, cap, 403, "git projects require a webchat user");

   cJSON *body = (rq->body && rq->body[0]) ? cJSON_Parse(rq->body) : NULL;
   if (!body || !cJSON_IsObject(body))
   {
      cJSON_Delete(body);
      return err_json(resp, cap, 400, "invalid JSON body");
   }
   const cJSON *jurl = cJSON_GetObjectItemCaseSensitive(body, "url");
   const cJSON *jname = cJSON_GetObjectItemCaseSensitive(body, "name");
   const cJSON *jorg = cJSON_GetObjectItemCaseSensitive(body, "org");
   const cJSON *jtoken = cJSON_GetObjectItemCaseSensitive(body, "token");
   const char *url = (cJSON_IsString(jurl) && jurl->valuestring) ? jurl->valuestring : NULL;
   const char *name = (cJSON_IsString(jname) && jname->valuestring) ? jname->valuestring : NULL;
   /* An empty-string org (the webchat relay always sends the field) means
    * "derive", identical to an absent field. */
   const char *org = (cJSON_IsString(jorg) && jorg->valuestring[0]) ? jorg->valuestring : NULL;
   const char *token = (cJSON_IsString(jtoken) && jtoken->valuestring) ? jtoken->valuestring : NULL;

   if (!util_url_is_remote(url))
   {
      cJSON_Delete(body);
      return err_json(resp, cap, 400, "clone url must be an http(s), ssh, or git remote");
   }

   char dest[MAX_PATH_LEN], pname[GIT_PROJECT_NAME_MAX], err[256];
   int rc = git_project_clone(principal, url, name, org, token, dest, sizeof(dest), pname,
                              sizeof(pname), err, sizeof(err));
   /* A multi-segment owner (GitLab subgroups) bails to a flat clone by design;
    * tell the caller how to place it under an org explicitly. (Checked before
    * body teardown — url points into it.) */
   int flat_multi = 0;
   if (rc == 0 && !org)
   {
      int multi = 0;
      char cand[65];
      if (git_project_derive_org(url, cand, sizeof(cand), &multi) != 0 && multi)
         flat_multi = 1;
   }
   char org_cands[256] = "";
   if (flat_multi)
      (void)git_project_org_candidates(url, org_cands, sizeof(org_cands));
   cJSON_Delete(body);
   if (rc != 0)
      /* Identity conflicts (existing project, flat/org clash, same key bound
       * to a different remote) are 409; validation failures stay 400. */
      return err_json(resp, cap, rc == GP_ERR_CONFLICT ? 409 : 400, err);

   /* Best-effort index so the new project is searchable by the agent + listed. */
   index_scan_project(pname, dest, 0);

   cJSON *out = cJSON_CreateObject();
   cJSON_AddBoolToObject(out, "ok", 1);
   cJSON_AddStringToObject(out, "name", pname); /* the project REF (org/repo or flat) */
   if (flat_multi)
   {
      char note[512];
      snprintf(note, sizeof(note),
               "multi-segment owner path (candidate orgs: %s): cloned flat; pass an explicit "
               "'org' to place it under an org",
               org_cands[0] ? org_cands : "none derivable");
      cJSON_AddStringToObject(out, "org_note", note);
   }
   rh_clone_kb_scan(pname, dest, out);
   char *s = cJSON_PrintUnformatted(out);
   int n = s ? snprintf(resp, (size_t)cap, "%s", s) : -1;
   free(s);
   cJSON_Delete(out);
   return (n > 0 && n < cap) ? 200 : err_json(resp, cap, 500, "response too large");
}

/* GET /v1/workspace/org-repos?host=&owner= — list the repositories under an owner
 * on a git host (provider-agnostic: GitHub/GitLab/Gitea/Bitbucket), so the wizard
 * can bulk-clone a workspace. Auth is the attested webuser principal; enumeration
 * uses the per-host token from the sealed vault (or unauthenticated for a public
 * org). Nothing is cloned here. */
int rh_workspace_org_repos(const route_req_t *rq, char *resp, int cap)
{
   (void)rq;
   if (!git_surface_enabled())
      return err_json(resp, cap, 503, "the git surface is disabled on this server");
   const char *principal = server_http_identity_principal();
   if (!principal || strncmp(principal, "webuser:", 8) != 0)
      return err_json(resp, cap, 403, "git projects require a webchat user");

   char host[256], owner[192];
   rh_query_str("host", host, sizeof(host));
   rh_query_str("owner", owner, sizeof(owner));

   cJSON *repos = NULL;
   char provider[32], err[256];
   int st = git_org_repos_list(host, owner, &repos, provider, sizeof(provider), err, sizeof(err));
   if (st != 0)
      return err_json(resp, cap, st, err);

   cJSON *out = cJSON_CreateObject();
   cJSON_AddStringToObject(out, "provider", provider);
   cJSON_AddItemToObject(out, "repos", repos); /* transfers ownership */
   char *s = cJSON_PrintUnformatted(out);
   int n = s ? snprintf(resp, (size_t)cap, "%s", s) : -1;
   free(s);
   cJSON_Delete(out);
   return (n > 0 && n < cap) ? 200 : err_json(resp, cap, 500, "response too large");
}

/* POST /v1/workspace/clone-org {host, owner, repos:[{name, clone_url}]} — bulk
 * clone a selection of a workspace's repos into the calling webuser's scoped tree.
 * Each repo is cloned via git_project_clone (identity + credential handling as
 * /v1/workspace/clone); individual failures do not abort the batch. Returns a
 * per-repo result list. */
int rh_workspace_clone_org(const route_req_t *rq, char *resp, int cap)
{
   if (!git_surface_enabled())
      return err_json(resp, cap, 503, "the git surface is disabled on this server");
   const char *principal = server_http_identity_principal();
   if (!principal || strncmp(principal, "webuser:", 8) != 0)
      return err_json(resp, cap, 403, "git projects require a webchat user");

   cJSON *body = (rq->body && rq->body[0]) ? cJSON_Parse(rq->body) : NULL;
   if (!body || !cJSON_IsObject(body))
   {
      cJSON_Delete(body);
      return err_json(resp, cap, 400, "invalid JSON body");
   }
   const cJSON *jrepos = cJSON_GetObjectItemCaseSensitive(body, "repos");
   if (!cJSON_IsArray(jrepos) || cJSON_GetArraySize(jrepos) == 0)
   {
      cJSON_Delete(body);
      return err_json(resp, cap, 400, "repos[] required");
   }
   if (cJSON_GetArraySize(jrepos) > 100)
   {
      cJSON_Delete(body);
      return err_json(resp, cap, 400, "too many repos (max 100 per request)");
   }

   /* The org the operator was BROWSING, used only as a fallback. It is not the
    * owner of every repo in the batch: enumerating an account returns repos it
    * merely has access to, so a games-on-whales repo can arrive in a batch the
    * wizard labelled JBailes. Filing all of them under the browsed owner put 15
    * repos in the wrong org directory on a real appliance —
    * webusers/admin/JBailes/discowolf whose remote is games-on-whales/discowolf,
    * a repo JBailes does not own at all. The Projects page groups by that
    * directory, so it then reported the wrong org for those repos and the org it
    * was browsing looked as though the clone had never happened.
    *
    * Each repo's real owner is in its own clone_url; derive it per repo below. */
   const cJSON *jowner = cJSON_GetObjectItemCaseSensitive(body, "owner");
   const char *browsed_owner =
       (cJSON_IsString(jowner) && jowner->valuestring[0]) ? jowner->valuestring : NULL;

   cJSON *out = cJSON_CreateObject();
   cJSON *results = cJSON_AddArrayToObject(out, "results");
   const cJSON *jrepo = NULL;
   cJSON_ArrayForEach(jrepo, jrepos)
   {
      const cJSON *jname = cJSON_GetObjectItemCaseSensitive(jrepo, "name");
      const cJSON *jurl = cJSON_GetObjectItemCaseSensitive(jrepo, "clone_url");
      const char *name = (cJSON_IsString(jname) && jname->valuestring) ? jname->valuestring : NULL;
      const char *url = (cJSON_IsString(jurl) && jurl->valuestring) ? jurl->valuestring : NULL;

      cJSON *r = cJSON_CreateObject();
      cJSON_AddStringToObject(r, "name", name ? name : "");
      char dest[MAX_PATH_LEN], pname[GIT_PROJECT_NAME_MAX], err[256];
      /* Same untrusted-input rule as /v1/workspace/clone: a batch entry must
       * also name a real remote, so one crafted clone_url cannot smuggle a
       * local path in through the bulk route. */
      int remote_ok = util_url_is_remote(url);
      /* This repo's own owner, from its own URL. Falls back to the browsed owner
       * only when the URL yields no single-segment owner (a GitLab subgroup),
       * which is the case git_project_clone flattens by design. */
      char repo_org[GIT_PROJECT_NAME_MAX];
      int multi = 0;
      const char *org = browsed_owner;
      if (remote_ok && git_project_derive_org(url, repo_org, sizeof(repo_org), &multi) == 0)
         org = repo_org;
      /* token=NULL → the host's stored credential (or server identity) is used. */
      int rc = remote_ok ? git_project_clone(principal, url, name, org, NULL, dest, sizeof(dest),
                                             pname, sizeof(pname), err, sizeof(err))
                         : -1;
      if (rc == 0)
      {
         index_scan_project(pname, dest, 0); /* best-effort: make it searchable */
         cJSON_AddBoolToObject(r, "ok", 1);
         cJSON_AddStringToObject(r, "project", pname);
         cJSON_AddNullToObject(r, "error");
         rh_clone_kb_scan(pname, dest, r);
      }
      else
      {
         cJSON_AddBoolToObject(r, "ok", 0);
         cJSON_AddNullToObject(r, "project");
         cJSON_AddStringToObject(
             r, "error",
             !url ? "missing clone_url"
                  : (!remote_ok ? "clone_url must be an http(s), ssh, or git remote" : err));
      }
      cJSON_AddItemToArray(results, r);
   }
   cJSON_Delete(body);

   char *s = cJSON_PrintUnformatted(out);
   int n = s ? snprintf(resp, (size_t)cap, "%s", s) : -1;
   free(s);
   cJSON_Delete(out);
   return (n > 0 && n < cap) ? 200 : err_json(resp, cap, 500, "response too large");
}

/* POST /v1/workspace/git {project, op, message?, branch?, n?} — run a git
 * operation on the calling webchat user's project (webchat-git WP-E). Identity
 * is the attested X-Aimee-Webuser principal; the project + op are scoped +
 * allowlisted by git_ops, and remote ops use the user's vaulted creds. */
int rh_workspace_git(const route_req_t *rq, char *resp, int cap)
{
   if (!git_surface_enabled())
      return err_json(resp, cap, 503, "the git surface is disabled on this server");
   const char *principal = server_http_identity_principal();
   if (!principal || strncmp(principal, "webuser:", 8) != 0)
      return err_json(resp, cap, 403, "git projects require a webchat user");

   cJSON *body = (rq->body && rq->body[0]) ? cJSON_Parse(rq->body) : NULL;
   if (!body || !cJSON_IsObject(body))
   {
      cJSON_Delete(body);
      return err_json(resp, cap, 400, "invalid JSON body");
   }
   const cJSON *jproj = cJSON_GetObjectItemCaseSensitive(body, "project");
   const cJSON *jop = cJSON_GetObjectItemCaseSensitive(body, "op");
   const cJSON *jmsg = cJSON_GetObjectItemCaseSensitive(body, "message");
   const cJSON *jbranch = cJSON_GetObjectItemCaseSensitive(body, "branch");
   const cJSON *jn = cJSON_GetObjectItemCaseSensitive(body, "n");
   const cJSON *jsid = cJSON_GetObjectItemCaseSensitive(body, "session_id");
   const char *project = (cJSON_IsString(jproj) && jproj->valuestring) ? jproj->valuestring : NULL;
   const char *op = (cJSON_IsString(jop) && jop->valuestring) ? jop->valuestring : NULL;
   /* Optional: run in the calling session's isolated worktree (same tree its
    * agent edits) rather than the shared project checkout. */
   const char *session_id = (cJSON_IsString(jsid) && jsid->valuestring) ? jsid->valuestring : NULL;
   /* text_arg is the commit message (commit), target branch (checkout), or the
    * PR title (pr; optional — empty → gh --fill from the branch's commits). */
   const char *text = NULL;
   if (op && (strcmp(op, "commit") == 0 || strcmp(op, "pr") == 0))
      text = (cJSON_IsString(jmsg) && jmsg->valuestring) ? jmsg->valuestring : NULL;
   else if (op && strcmp(op, "checkout") == 0)
      text = (cJSON_IsString(jbranch) && jbranch->valuestring) ? jbranch->valuestring : NULL;
   int num = (cJSON_IsNumber(jn)) ? (int)jn->valuedouble : 0;

   char *git_out = NULL, err[256];
   int rc = git_ops_run_session(principal, project, session_id, op, text, num, &git_out, err,
                                sizeof(err));
   cJSON_Delete(body);
   if (rc != 0)
   {
      free(git_out);
      return err_json(resp, cap, 400, err);
   }
   cJSON *out = cJSON_CreateObject();
   cJSON_AddBoolToObject(out, "ok", 1);
   cJSON_AddStringToObject(out, "output", git_out ? git_out : "");
   free(git_out);
   char *s = cJSON_PrintUnformatted(out);
   int n = s ? snprintf(resp, (size_t)cap, "%s", s) : -1;
   free(s);
   cJSON_Delete(out);
   return (n > 0 && n < cap) ? 200 : err_json(resp, cap, 500, "git output too large");
}

static int wfe_ref_valid(const char *s)
{
   if (!s || !s[0] || s[0] == '-' || strlen(s) > 200 || strstr(s, ".."))
      return 0;
   for (const unsigned char *p = (const unsigned char *)s; *p; p++)
      if (!(isalnum(*p) || *p == '.' || *p == '_' || *p == '/' || *p == '-'))
         return 0;
   return 1;
}

#define WFE_MAX_ID_LEN 80

static int wfe_id_root_char(unsigned char c)
{
   return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}

static int wfe_item_id_valid(const char *s)
{
   if (!s || strncmp(s, "wi_", 3) != 0 || strlen(s) > WFE_MAX_ID_LEN)
      return 0;
   const unsigned char *p = (const unsigned char *)s + 3;
   const unsigned char *root = p;
   while (wfe_id_root_char(*p))
      p++;
   if (p == root)
      return 0;
   if (!*p)
      return 1;
   if (p[0] != '.' || p[1] != 's')
      return 0;
   p += 2;
   for (int i = 0; i < 10; i++, p++)
      if (!isdigit(*p) && (*p < 'a' || *p > 'f'))
         return 0;
   if (p[0] != '.' || p[1] != 'g')
      return 0;
   p += 2;
   if (!isdigit(*p))
      return 0;
   while (isdigit(*p))
      p++;
   if (*p++ != '.' || !isdigit(*p))
      return 0;
   while (isdigit(*p))
      p++;
   return *p == '\0';
}

static char *const wfe_git_env[] = {"PATH=/usr/local/bin:/usr/bin:/bin", "GIT_CONFIG_NOSYSTEM=1",
                                    "GIT_CONFIG_SYSTEM=/dev/null", "GIT_CONFIG_GLOBAL=/dev/null",
                                    NULL};

static pthread_once_t wfe_workspace_once = PTHREAD_ONCE_INIT;
/* A workspace-root relocation takes effect on process restart, never midway
 * through credential-bearing work. */
static char wfe_workspace_real[MAX_PATH_LEN] = {0};

static void wfe_workspace_init(void)
{
   const char *root = getenv("AIMEE_WORKSPACES_DIR");
   if (!root || !root[0])
      root = "/var/lib/aimee-workspaces";
   if (!realpath(root, wfe_workspace_real))
      wfe_workspace_real[0] = '\0';
}

static int wfe_git_capture(const char *dir, const char *const argv[], char *out, size_t out_cap)
{
   char *captured = NULL;
   int rc =
       safe_exec_capture_cwd_env_fd_timeout(argv, dir, wfe_git_env, &captured, 65536, 5000, -1, -1);
   if (rc != 0 || !captured)
   {
      free(captured);
      return -1;
   }
   captured[strcspn(captured, "\r\n")] = '\0';
   int ok = captured[0] && (size_t)snprintf(out, out_cap, "%s", captured) < out_cap;
   free(captured);
   return ok ? 0 : -1;
}

static int wfe_managed_repo(const char *workdir_in, const char *head, char *workdir,
                            size_t work_cap, char *trusted_repo, size_t repo_cap, char *remote,
                            size_t remote_cap, struct stat *identity, char *err, size_t errlen)
{
   (void)work_cap; /* callers provide MAX_PATH_LEN; realpath requires PATH_MAX storage */
   char prefix[MAX_PATH_LEN], top[MAX_PATH_LEN], common_raw[MAX_PATH_LEN], common[MAX_PATH_LEN];
   if (!workdir_in || !realpath(workdir_in, workdir))
   {
      snprintf(err, errlen, "managed worktree path does not exist");
      return -1;
   }
   if ((size_t)snprintf(prefix, sizeof(prefix), "%s/wfe-worktrees/", aimee_home()) >=
           sizeof(prefix) ||
       strncmp(workdir, prefix, strlen(prefix)) != 0)
   {
      snprintf(err, errlen, "worktree is outside the managed WFE root");
      return -1;
   }
   const char *item_id = workdir + strlen(prefix);
   if (!wfe_item_id_valid(item_id) || strchr(item_id, '/'))
   {
      snprintf(err, errlen, "invalid managed worktree id");
      return -1;
   }
   if (head)
   {
      char feature[300], slice[300];
      snprintf(feature, sizeof(feature), "aimee/feat/%s", item_id);
      snprintf(slice, sizeof(slice), "aimee/wi/%s", item_id);
      int slice_item = strstr(item_id, ".s") != NULL;
      if ((slice_item && strcmp(head, slice) != 0) || (!slice_item && strcmp(head, feature) != 0))
      {
         snprintf(err, errlen, "managed branch does not match work-item id");
         return -1;
      }
   }
   struct stat st;
   const char *top_argv[] = {"git",   "-c",        "core.hooksPath=/dev/null", "-C",
                             workdir, "rev-parse", "--show-toplevel",          NULL};
   char *top_out = NULL;
   if (lstat(workdir_in, &st) != 0 || S_ISLNK(st.st_mode) || !S_ISDIR(st.st_mode) ||
       safe_exec_capture_cwd_env_timeout(top_argv, workdir, wfe_git_env, &top_out, 4096, 5000) !=
           0 ||
       !top_out)
   {
      free(top_out);
      snprintf(err, errlen, "invalid managed git worktree");
      return -1;
   }
   if (identity)
      *identity = st;
   const char *common_argv[] = {
       "git", "-C", workdir, "rev-parse", "--path-format=absolute", "--git-common-dir", NULL};
   if (wfe_git_capture(workdir, common_argv, common_raw, sizeof(common_raw)) != 0 ||
       !realpath(common_raw, common))
   {
      snprintf(err, errlen, "cannot resolve managed repository identity");
      return -1;
   }
   size_t common_len = strlen(common);
   if (common_len <= 5 || strcmp(common + common_len - 5, "/.git") != 0)
   {
      snprintf(err, errlen, "managed worktree has an invalid common directory");
      return -1;
   }
   common[common_len - 5] = '\0';
   if (!realpath(common, trusted_repo))
   {
      snprintf(err, errlen, "managed repository identity is unavailable");
      return -1;
   }
   pthread_once(&wfe_workspace_once, wfe_workspace_init);
   if (!wfe_workspace_real[0])
   {
      snprintf(err, errlen, "workspace root is unavailable");
      return -1;
   }
   size_t workspace_len = strlen(wfe_workspace_real);
   if (strncmp(trusted_repo, wfe_workspace_real, workspace_len) != 0 ||
       trusted_repo[workspace_len] != '/')
   {
      snprintf(err, errlen, "managed repository is outside the workspace root");
      return -1;
   }
   top_out[strcspn(top_out, "\r\n")] = '\0';
   snprintf(top, sizeof(top), "%s", top_out);
   free(top_out);
   if (strcmp(top, workdir) != 0)
   {
      snprintf(err, errlen, "git top-level does not match managed worktree");
      return -1;
   }
   if (head)
   {
      const char *branch_argv[] = {"git", "-C", workdir, "rev-parse", "--abbrev-ref", "HEAD", NULL};
      char branch[256];
      if (wfe_git_capture(workdir, branch_argv, branch, sizeof(branch)) != 0)
      {
         snprintf(err, errlen, "cannot resolve managed branch");
         return -1;
      }
      int match = strcmp(branch, head) == 0;
      if (!match)
      {
         snprintf(err, errlen, "managed branch mismatch");
         return -1;
      }
   }
   if (git_pr_https_origin_url(trusted_repo, remote, remote_cap, err, errlen) != 0)
   {
      snprintf(err, errlen, "managed repository has no supported origin");
      return -1;
   }
   return 0;
}

/* A final WFE PR targets the branch on which the admitted repository was
 * checked out (for example `testing`), not necessarily GitHub's repository
 * default (`main`). Binding the base to this trusted checkout prevents an item
 * from selecting an arbitrary remote branch while preserving non-default
 * integration lanes. */
static int wfe_managed_base(const char *repo, const char *base, char *err, size_t errlen)
{
   char branch[256];
   const char *branch_argv[] = {"git", "-C", repo, "rev-parse", "--abbrev-ref", "HEAD", NULL};
   if (wfe_git_capture(repo, branch_argv, branch, sizeof(branch)) != 0 || !branch[0] ||
       strcmp(branch, "HEAD") == 0)
   {
      snprintf(err, errlen, "cannot resolve managed integration branch");
      return -1;
   }
   return strcmp(branch, base) == 0;
}

static int wfe_forge_body_fields_valid(const cJSON *body)
{
   static const char *const allowed[] = {"op",   "workdir", "head",   "base", "title",
                                         "body", "draft",   "number", NULL};
   for (const cJSON *field = body ? body->child : NULL; field; field = field->next)
   {
      int index = -1;
      for (size_t i = 0; allowed[i]; i++)
         if (field->string && strcmp(field->string, allowed[i]) == 0)
         {
            index = (int)i;
            break;
         }
      if (index < 0)
         return 0;
      for (const cJSON *prior = body->child; prior != field; prior = prior->next)
         if (prior->string && strcmp(prior->string, field->string) == 0)
            return 0;
      int valid_type = index == 7   ? cJSON_IsNumber(field)
                       : index == 6 ? cJSON_IsBool(field)
                                    : cJSON_IsString(field);
      if (!valid_type)
         return 0;
   }
   return 1;
}

static int wfe_slice_ref_matches_workdir(const char *workdir, const char *prefix, int parent_only,
                                         const char *ref)
{
   const char *item_id = strrchr(workdir, '/');
   item_id = item_id ? item_id + 1 : workdir;
   if (!wfe_item_id_valid(item_id))
      return 0;
   const char *slice_suffix = strstr(item_id, ".s");
   if (!slice_suffix)
      return 0;
   size_t item_len = parent_only ? (size_t)(slice_suffix - item_id) : strlen(item_id);
   char expected[300];
   int n = snprintf(expected, sizeof(expected), "%s%.*s", prefix, (int)item_len, item_id);
   return n > 0 && (size_t)n < sizeof(expected) && strcmp(expected, ref) == 0;
}

static int wfe_forge_operation_valid(const char *op, const char *head, const char *base,
                                     const char *title, const char *body, const cJSON *jdraft,
                                     int draft, const cJSON *jnumber, int number)
{
   int has_number = jnumber != NULL;
   int has_draft = jdraft != NULL;
   if (has_number && (!cJSON_IsNumber(jnumber) || jnumber->valuedouble != (double)number))
      return 0;
   if (strcmp(op, "push") == 0)
      return head && !base && !title && !body && !has_draft && !has_number;
   if (strcmp(op, "identity") == 0)
      return !head && !base && !title && !body && !has_draft && !has_number;
   if (strcmp(op, "open") == 0)
   {
      int final_head = head && strncmp(head, "aimee/feat/wi_", 14) == 0;
      return head && base && title && title[0] && body && body[0] && has_draft &&
             cJSON_IsBool(jdraft) && draft == final_head && !has_number;
   }
   if (strcmp(op, "info") == 0 || strcmp(op, "ci") == 0)
      return !head && !base && !title && !body && !has_draft && has_number && number > 0;
   if (strcmp(op, "merge") == 0)
      return !head && base && !title && !body && !has_draft && has_number && number > 0;
   return 0;
}

/* Mechanical authenticated forge execution for Go-owned WFE state. The raw
 * credential never crosses this process boundary. C validates only structural
 * security invariants and executes the explicit operation; it does not inspect
 * DB1, interpret workflows, choose an operation, or advance a lifecycle. */
int rh_internal_forge_execute(const route_req_t *rq, char *resp, int cap)
{
   const char *principal = server_http_identity_principal();
   if (!principal || strncmp(principal, "uid:", 4) != 0)
      return err_json(resp, cap, 403, "local Unix peer required");
   cJSON *body = (rq->body && rq->body[0]) ? cJSON_Parse(rq->body) : NULL;
   const char *op = route_json_string(body, "op");
   const char *workdir_in = route_json_string(body, "workdir");
   const char *head = route_json_string(body, "head");
   const char *base = route_json_string(body, "base");
   const char *title = route_json_string(body, "title");
   const char *pr_body = route_json_string(body, "body");
   const cJSON *jdraft = body ? cJSON_GetObjectItemCaseSensitive(body, "draft") : NULL;
   int draft = cJSON_IsTrue(jdraft) ? 1 : 0;
   const cJSON *jnumber = body ? cJSON_GetObjectItemCaseSensitive(body, "number") : NULL;
   int number = cJSON_IsNumber(jnumber) ? jnumber->valueint : 0;
   if (!body || !wfe_forge_body_fields_valid(body) || !op || !workdir_in ||
       (head && !wfe_ref_valid(head)) || (base && !wfe_ref_valid(base)) ||
       (title && strlen(title) > 256) || (pr_body && strlen(pr_body) > 60000) ||
       !wfe_forge_operation_valid(op, head, base, title, pr_body, jdraft, draft, jnumber, number))
   {
      cJSON_Delete(body);
      return err_json(resp, cap, 400, "invalid forge operation request");
   }
   char workdir[MAX_PATH_LEN], trusted_repo[MAX_PATH_LEN], remote[512], err[256];
   struct stat identity;
   if (wfe_managed_repo(workdir_in, head, workdir, sizeof(workdir), trusted_repo,
                        sizeof(trusted_repo), remote, sizeof(remote), &identity, err,
                        sizeof(err)) != 0)
   {
      cJSON_Delete(body);
      return err_json(resp, cap, 403, err);
   }
   cJSON *out = cJSON_CreateObject();
   char *detail = NULL;
   int rc = -1;
   struct stat current;
   if (lstat(workdir, &current) != 0 || current.st_dev != identity.st_dev ||
       current.st_ino != identity.st_ino)
   {
      cJSON_Delete(body);
      cJSON_Delete(out);
      return err_json(resp, cap, 409, "managed worktree identity changed");
   }
   int feature_head = head && strncmp(head, "aimee/feat/wi_", 14) == 0;
   int slice_head = head && strncmp(head, "aimee/wi/wi_", 12) == 0;
   int allowed_push_head =
       feature_head || (slice_head && wfe_slice_ref_matches_workdir(workdir, "aimee/wi/", 0, head));
   if (strcmp(op, "identity") == 0)
   {
      char name[256], email[256];
      int identity_rc = git_identity_resolve(workdir, name, sizeof(name), email, sizeof(email));
      if (identity_rc < 0)
         snprintf(err, sizeof(err), "cannot read configured git identity");
      else
      {
         rc = 0;
         cJSON_AddBoolToObject(out, "configured", identity_rc == 1);
         if (identity_rc == 1)
         {
            cJSON_AddStringToObject(out, "name", name);
            cJSON_AddStringToObject(out, "email", email);
         }
      }
   }
   else if (strcmp(op, "push") == 0 && head && allowed_push_head)
   {
      rc = git_ops_push_dir(principal, workdir, remote, head, &detail, err, sizeof(err));
   }
   else if (strcmp(op, "open") == 0)
   {
      int base_ok = slice_head && wfe_slice_ref_matches_workdir(workdir, "aimee/feat/", 1, base);
      if (feature_head)
         base_ok = wfe_managed_base(trusted_repo, base, err, sizeof(err));
      if (base_ok == 0)
         snprintf(err, sizeof(err), "pull request base is outside the managed target");
      if (base_ok == 1)
      {
         char *push_out = NULL, url[1024];
         rc = git_ops_push_dir(principal, workdir, remote, head, &push_out, err, sizeof(err));
         if (rc != 0)
            detail = push_out;
         else
            free(push_out);
         if (rc == 0)
         {
            int existing_number = 0;
            int found = git_pr_find_open_via_api(principal, trusted_repo, head, base, url,
                                                 sizeof(url), &existing_number, err, sizeof(err));
            if (found == 0)
               rc = git_pr_create_via_api_ex_draft(principal, trusted_repo, head, base, title,
                                                   pr_body, draft, url, sizeof(url), err,
                                                   sizeof(err));
            else
               rc = found == 1 && existing_number > 0
                        ? git_pr_update_via_api(principal, trusted_repo, existing_number, title,
                                                pr_body, err, sizeof(err))
                        : -1;
            if (rc == 0)
               cJSON_AddStringToObject(out, "url", url);
         }
      }
   }
   else if (strcmp(op, "info") == 0 && number > 0)
   {
      git_pr_info_t info;
      rc = git_pr_info_via_api(principal, trusted_repo, number, &info, err, sizeof(err));
      if (rc == 0)
      {
         cJSON_AddBoolToObject(out, "open", info.open);
         cJSON_AddBoolToObject(out, "merged", info.merged);
         cJSON_AddNumberToObject(out, "mergeable", info.mergeable);
         cJSON_AddStringToObject(out, "base", info.base);
      }
   }
   else if (strcmp(op, "ci") == 0 && number > 0)
   {
      git_pr_ci_t ci = git_pr_ci_via_api(principal, trusted_repo, number, err, sizeof(err));
      rc = ci == GIT_PR_CI_ERROR ? -1 : 0;
      if (rc == 0)
      {
         const char *state = ci == GIT_PR_CI_PENDING   ? "pending"
                             : ci == GIT_PR_CI_FAILURE ? "failed"
                                                       : "passed";
         cJSON_AddStringToObject(out, "state", state);
      }
   }
   else if (strcmp(op, "merge") == 0 && number > 0 && base &&
            wfe_slice_ref_matches_workdir(workdir, "aimee/feat/", 1, base))
   {
      git_pr_info_t info;
      rc = git_pr_info_via_api(principal, trusted_repo, number, &info, err, sizeof(err));
      if (rc == 0 && (strcmp(info.base, base) != 0 ||
                      !wfe_slice_ref_matches_workdir(workdir, "aimee/wi/", 0, info.head)))
      {
         snprintf(err, sizeof(err), "pull request base mismatch");
         rc = -1;
      }
      if (rc == 0)
      {
         int merged = git_pr_merge_via_api(principal, trusted_repo, number, err, sizeof(err));
         rc = (merged == 0 || merged == 1) ? 0 : -1;
      }
   }
   else
      snprintf(err, sizeof(err), "unsupported forge operation");
   cJSON_Delete(body);
   if (rc != 0)
   {
      cJSON_Delete(out);
      cJSON *failure = cJSON_CreateObject();
      cJSON_AddStringToObject(failure, "error", err[0] ? err : "forge operation failed");
      if (detail && detail[0])
         cJSON_AddStringToObject(failure, "detail", detail);
      char *serialized = cJSON_PrintUnformatted(failure);
      cJSON_Delete(failure);
      free(detail);
      int n = serialized ? snprintf(resp, (size_t)cap, "%s", serialized) : -1;
      free(serialized);
      return (n > 0 && n < cap) ? 400 : err_json(resp, cap, 500, "forge error response too large");
   }
   free(detail);
   cJSON_AddBoolToObject(out, "ok", 1);
   char *serialized = cJSON_PrintUnformatted(out);
   cJSON_Delete(out);
   int n = serialized ? snprintf(resp, (size_t)cap, "%s", serialized) : -1;
   free(serialized);
   return (n > 0 && n < cap) ? 200 : err_json(resp, cap, 500, "forge response too large");
}

/* GET /v1/workspace/projects — list the calling webchat user's projects (the
 * repos cloned under their scoped workspace). Identity is the attested
 * X-Aimee-Webuser principal, so a user only ever sees their own. */
int rh_workspace_projects(const route_req_t *rq, char *resp, int cap)
{
   (void)rq;
   if (!git_surface_enabled())
      return err_json(resp, cap, 503, "the git surface is disabled on this server");
   const char *principal = server_http_identity_principal();
   if (!principal || strncmp(principal, "webuser:", 8) != 0)
      return err_json(resp, cap, 403, "git projects require a webchat user");

   char names[256][GIT_PROJECT_NAME_MAX];
   int count = git_project_list(principal, names, 256);
   if (count < 0)
      count = 0;
   cJSON *out = cJSON_CreateObject();
   /* `projects` stays an array of ref STRINGS (legacy consumers parse it and
    * ignore the sibling `details`); `details` adds org/name/remote per ref. */
   cJSON *arr = cJSON_AddArrayToObject(out, "projects");
   cJSON *details = cJSON_AddArrayToObject(out, "details");
   for (int i = 0; i < count; i++)
   {
      cJSON_AddItemToArray(arr, cJSON_CreateString(names[i]));
      cJSON *d = cJSON_CreateObject();
      cJSON_AddStringToObject(d, "ref", names[i]);
      const char *slash = strchr(names[i], '/');
      if (slash)
      {
         char org[GIT_PROJECT_NAME_MAX];
         snprintf(org, sizeof(org), "%.*s", (int)(slash - names[i]), names[i]);
         cJSON_AddStringToObject(d, "org", org);
         cJSON_AddStringToObject(d, "name", slash + 1);
      }
      else
      {
         cJSON_AddStringToObject(d, "org", "");
         cJSON_AddStringToObject(d, "name", names[i]);
      }
      char remote[1024];
      if (git_project_remote(principal, names[i], remote, sizeof(remote)) == 0)
         cJSON_AddStringToObject(d, "remote", remote);
      cJSON_AddItemToArray(details, d);
   }
   /* The user's scoped workspace root — used by the editor to open a project
    * folder (root/<project>) and by chat to set its working directory. It is the
    * caller's own workspace path, returned only to them. */
   char root[MAX_PATH_LEN];
   if (ws_scope_user_root(principal, 0, root, sizeof(root)) == 0)
      cJSON_AddStringToObject(out, "root", root);
   char *s = cJSON_PrintUnformatted(out);
   int n = s ? snprintf(resp, (size_t)cap, "%s", s) : -1;
   free(s);
   cJSON_Delete(out);
   return (n > 0 && n < cap) ? 200 : err_json(resp, cap, 500, "response too large");
}

/* POST /v1/workspace/projects/delete {ref} — delete a cloned project from this
 * environment (webchat project lifecycle proposal, slice 2). The ONLY identity
 * source is the attested X-Aimee-Webuser principal; an unresolvable ref is a
 * plain 404. The delete is LOCAL: the clone and this server's own state go, and
 * aimee-kb — a separate multi-tenant service — is not called, so there is no
 * purge outcome to report and nothing to force past. Capability: tool:execute. */
int rh_workspace_projects_delete(const route_req_t *rq, char *resp, int cap)
{
   if (!git_surface_enabled())
      return err_json(resp, cap, 503, "the git surface is disabled on this server");
   const char *principal = server_http_identity_principal();
   if (!principal || strncmp(principal, "webuser:", 8) != 0)
      return err_json(resp, cap, 403, "git projects require a webchat user");

   cJSON *body = (rq->body && rq->body[0]) ? cJSON_Parse(rq->body) : NULL;
   if (!body || !cJSON_IsObject(body))
   {
      cJSON_Delete(body);
      return err_json(resp, cap, 400, "invalid JSON body");
   }
   const cJSON *jref = cJSON_GetObjectItemCaseSensitive(body, "ref");
   char ref[GIT_PROJECT_NAME_MAX];
   ref[0] = '\0';
   if (cJSON_IsString(jref) && jref->valuestring && strlen(jref->valuestring) < sizeof(ref))
      snprintf(ref, sizeof(ref), "%s", jref->valuestring);
   cJSON_Delete(body);
   if (!ref[0])
      return err_json(resp, cap, 400, "ref required");

   char err[512];
   int rc = git_project_delete(principal, ref, err, sizeof(err));
   if (rc == GP_ERR_NOT_FOUND)
      return err_json(resp, cap, 404, "not found");
   if (rc != 0)
      return err_json(resp, cap, 400, err);

   cJSON *out = cJSON_CreateObject();
   cJSON_AddBoolToObject(out, "ok", 1);
   cJSON_AddStringToObject(out, "ref", ref);
   char *s = cJSON_PrintUnformatted(out);
   int n = s ? snprintf(resp, (size_t)cap, "%s", s) : -1;
   free(s);
   cJSON_Delete(out);
   return (n > 0 && n < cap) ? 200 : err_json(resp, cap, 500, "response too large");
}

/* POST /v1/workspace/session-dir {project, session_id} — resolve the absolute
 * directory the given session acts in for `project`: that session's isolated
 * sibling worktree (off the default branch, created on demand) when session_id is
 * present, else the project checkout. Lets the editor open the same tree the
 * session's agent edits. Identity is the attested webuser principal. */
int rh_workspace_session_dir(const route_req_t *rq, char *resp, int cap)
{
   if (!git_surface_enabled())
      return err_json(resp, cap, 503, "the git surface is disabled on this server");
   const char *principal = server_http_identity_principal();
   if (!principal || strncmp(principal, "webuser:", 8) != 0)
      return err_json(resp, cap, 403, "git projects require a webchat user");

   cJSON *body = (rq->body && rq->body[0]) ? cJSON_Parse(rq->body) : NULL;
   if (!body || !cJSON_IsObject(body))
   {
      cJSON_Delete(body);
      return err_json(resp, cap, 400, "invalid JSON body");
   }
   const cJSON *jproj = cJSON_GetObjectItemCaseSensitive(body, "project");
   const cJSON *jsid = cJSON_GetObjectItemCaseSensitive(body, "session_id");
   const char *project = (cJSON_IsString(jproj) && jproj->valuestring) ? jproj->valuestring : NULL;
   const char *session_id = (cJSON_IsString(jsid) && jsid->valuestring) ? jsid->valuestring : NULL;

   char dir[MAX_PATH_LEN], err[256];
   int rc = git_ops_session_dir(principal, project, session_id, dir, sizeof(dir), err, sizeof(err));
   cJSON_Delete(body);
   if (rc != 0)
      return err_json(resp, cap, 400, err);
   cJSON *out = cJSON_CreateObject();
   cJSON_AddStringToObject(out, "dir", dir);
   char *s = cJSON_PrintUnformatted(out);
   int n = s ? snprintf(resp, (size_t)cap, "%s", s) : -1;
   free(s);
   cJSON_Delete(out);
   return (n > 0 && n < cap) ? 200 : err_json(resp, cap, 500, "response too large");
}

/* POST /v1/workspace/editor — ensure the calling webchat user's code-server is
 * running and return its loopback port (webchat-git WP-I). Identity is the
 * attested X-Aimee-Webuser principal, so a user only ever drives their own
 * editor, rooted at their scoped workspace and launched with their vault-backed
 * git env. The port reaches only webchat (the trusted reverse-proxy, WP-J),
 * never the browser. 503 when the feature is disabled / code-server absent. */
int rh_workspace_editor(const route_req_t *rq, char *resp, int cap)
{
   (void)rq;
   const char *principal = server_http_identity_principal();
   if (!principal || strncmp(principal, "webuser:", 8) != 0)
      return err_json(resp, cap, 403, "the editor requires a webchat user");

   int port = 0;
   char err[256];
   int rc = webuser_editor_ensure(principal, &port, err, sizeof(err));
   if (rc == 0)
      return err_json(resp, cap, 503, "editor not available");
   if (rc < 0)
      return err_json(resp, cap, 500, err[0] ? err : "failed to start editor");

   cJSON *out = cJSON_CreateObject();
   cJSON_AddBoolToObject(out, "ok", 1);
   cJSON_AddNumberToObject(out, "port", port);
   char *s = cJSON_PrintUnformatted(out);
   int n = s ? snprintf(resp, (size_t)cap, "%s", s) : -1;
   free(s);
   cJSON_Delete(out);
   return (n > 0 && n < cap) ? 200 : err_json(resp, cap, 500, "response too large");
}

/* Per-host git credentials (single-user server, many providers). The calling
 * webchat user manages aimee-server's OWN stored tokens (one per host); the
 * secret is write-only over the API — listing returns host names only, never
 * tokens. Identity is the attested X-Aimee-Webuser principal. */
int rh_git_credentials(const route_req_t *rq, char *resp, int cap)
{
   if (!git_surface_enabled())
      return err_json(resp, cap, 503, "the git surface is disabled on this server");
   const char *principal = server_http_identity_principal();
   if (!principal || strncmp(principal, "webuser:", 8) != 0)
      return err_json(resp, cap, 403, "git credentials require a webchat user");

   /* GET → list configured hosts (no tokens). */
   if (strcmp(rq->method, "GET") == 0)
   {
      char hosts[64][GIT_HOST_MAX];
      int count = git_host_cred_list(hosts, 64);
      if (count < 0)
         count = 0;
      cJSON *out = cJSON_CreateObject();
      cJSON *arr = cJSON_AddArrayToObject(out, "hosts");
      for (int i = 0; i < count; i++)
         cJSON_AddItemToArray(arr, cJSON_CreateString(hosts[i]));
      char *s = cJSON_PrintUnformatted(out);
      int n = s ? snprintf(resp, (size_t)cap, "%s", s) : -1;
      free(s);
      cJSON_Delete(out);
      return (n > 0 && n < cap) ? 200 : err_json(resp, cap, 500, "response too large");
   }

   cJSON *body = (rq->body && rq->body[0]) ? cJSON_Parse(rq->body) : NULL;
   if (!body || !cJSON_IsObject(body))
   {
      cJSON_Delete(body);
      return err_json(resp, cap, 400, "invalid JSON body");
   }
   const cJSON *jhost = cJSON_GetObjectItemCaseSensitive(body, "host");
   const cJSON *jtoken = cJSON_GetObjectItemCaseSensitive(body, "token");
   const char *host = (cJSON_IsString(jhost) && jhost->valuestring) ? jhost->valuestring : NULL;
   const char *token = (cJSON_IsString(jtoken) && jtoken->valuestring) ? jtoken->valuestring : NULL;
   /* Accept a full URL in `host` too (convenience) → reduce to its host. */
   char hostbuf[GIT_HOST_MAX];
   if (host && strstr(host, "://") && git_host_from_url(host, hostbuf, sizeof(hostbuf)))
      host = hostbuf;
   if (!host || !host[0])
   {
      cJSON_Delete(body);
      return err_json(resp, cap, 400, "host required");
   }

   int rc;
   if (strcmp(rq->method, "DELETE") == 0)
   {
      rc = git_host_cred_delete(host);
      /* Revocation must leave no live credential handle behind (G5): the user's
       * running code-server baked the now-removed token into its env at spawn,
       * and the ssh-agent may hold a key loaded from the vault. Drop both AFTER
       * the vault entry is gone — order matters: deleting first means a /vscode
       * request that respawns the editor between these two steps re-reads an
       * already-empty vault, so it cannot pick the token back up. Both stop
       * functions are void + idempotent (no-op if nothing is running) so a
       * concurrent double-revoke is safe; webuser_editor_stop reaps the child
       * synchronously (bounded ~500ms), git_ssh_agent_stop only signals+unlinks.
       * Recycling is cheap — the editor respawns lazily on the next /vscode
       * request with a freshly-built (credential-free) env. */
      if (rc == 0)
      {
         webuser_editor_stop(principal);
         git_ssh_agent_stop(principal);
      }
   }
   else /* POST → set */
   {
      if (!token || !token[0])
      {
         cJSON_Delete(body);
         return err_json(resp, cap, 400, "token required");
      }
      rc = git_host_cred_set(host, token);
   }
   cJSON_Delete(body);
   if (rc != 0)
      return err_json(resp, cap, 500, "credential store failed");
   return snprintf(resp, (size_t)cap, "{\"ok\":true}") < cap
              ? 200
              : err_json(resp, cap, 500, "too large");
}

/* Per-webuser SSH private key for git over SSH. Stored under the caller's OWN
 * principal (webuser:<name>, "git", "ssh_key") but sealed with the SERVER master
 * KEK (a server-only wrap), so the server can load it into the user's in-memory
 * ssh-agent autonomously (git_ssh_agent_ensure → git_forge_vault_sshkey) AND
 * storing it needs NO vault unlock — parity with per-host git tokens and delegate
 * keys. The key never reaches the browser (write-only) and never lands on disk.
 * The secret is never logged. */
int rh_git_sshkey(const route_req_t *rq, char *resp, int cap)
{
   if (!git_surface_enabled())
      return err_json(resp, cap, 503, "the git surface is disabled on this server");
   const char *principal = server_http_identity_principal();
   if (!principal || strncmp(principal, "webuser:", 8) != 0)
      return err_json(resp, cap, 403, "ssh keys require a webchat user");

   if (strcmp(rq->method, "DELETE") == 0)
   {
      /* Remove the stored key and drop any live agent handle (mirrors revoke). A
       * missing entry is success — DELETE is idempotent. git_ssh_agent_stop runs
       * only on a clean delete: on a real vault error the persisted key still
       * exists, so tearing the agent down would just force a reload — leaving it
       * is the less-inconsistent state. */
      vault_status_t st =
          vault_service_delete(principal, GIT_FORGE_VAULT_AGENT, GIT_FORGE_SSHKEY_CRED);
      if (st != VAULT_OK && st != VAULT_NO_ENTRY)
         return err_json(resp, cap, 500, "vault delete failed");
      git_ssh_agent_stop(principal);
      return snprintf(resp, (size_t)cap, "{\"ok\":true}") < cap
                 ? 200
                 : err_json(resp, cap, 500, "too large");
   }
   if (strcmp(rq->method, "POST") != 0)
      return err_json(resp, cap, 405, "method not allowed");

   /* Bound the body before parsing: a private key is a few KB, so anything past
    * 64 KiB is abuse — fail fast rather than parse + re-wrap a huge blob. */
   if (rq->body_len > 65536)
      return err_json(resp, cap, 413, "ssh key too large");

   cJSON *body = (rq->body && rq->body[0]) ? cJSON_Parse(rq->body) : NULL;
   if (!body || !cJSON_IsObject(body))
   {
      cJSON_Delete(body);
      return err_json(resp, cap, 400, "invalid JSON body");
   }
   const cJSON *jkey = cJSON_GetObjectItemCaseSensitive(body, "ssh_key");
   const char *key = (cJSON_IsString(jkey) && jkey->valuestring) ? jkey->valuestring : NULL;
   /* Cheap shape check so an obviously-wrong paste fails fast with a clear
    * message; ssh-add is still the authority at load time. Anchor on the PEM/
    * OpenSSH armor ("-----BEGIN … PRIVATE KEY-----") rather than a loose
    * substring so arbitrary text containing the words can't slip through. We do
    * NOT accept a passphrase-encrypted key — the agent loads non-interactively. */
   if (!key || strncmp(key, "-----BEGIN", 10) != 0 || !strstr(key, "PRIVATE KEY-----"))
   {
      cJSON_Delete(body);
      return err_json(resp, cap, 400, "an unencrypted OpenSSH/PEM private key is required");
   }
   if (strstr(key, "ENCRYPTED") || strstr(key, "Proc-Type:"))
   {
      cJSON_Delete(body);
      return err_json(resp, cap, 400,
                      "passphrase-encrypted keys are not supported; provide an unencrypted key");
   }

   /* Seal the key under the SERVER master KEK (not the caller's per-user KEK) so
    * storing needs NO vault unlock — parity with per-host git tokens and delegate
    * keys. It is read back the same way (git_forge_vault_sshkey ->
    * vault_service_get_server_wrap), so there is never a VAULT_ERR_LOCKED here. */
   vault_status_t st =
       vault_service_set_server_wrap(principal, GIT_FORGE_VAULT_AGENT, GIT_FORGE_SSHKEY_CRED, key);
   /* Zero our parsed copy of the secret before freeing the JSON tree. */
   if (jkey && jkey->valuestring)
   {
      volatile char *p = (volatile char *)jkey->valuestring;
      for (size_t i = 0; p[i]; i++)
         p[i] = 0;
   }
   cJSON_Delete(body);
   if (st != VAULT_OK)
      return err_json(resp, cap, 500, "vault store failed");
   /* A freshly stored key supersedes any agent already running with the old one. */
   git_ssh_agent_stop(principal);
   return snprintf(resp, (size_t)cap, "{\"ok\":true}") < cap
              ? 200
              : err_json(resp, cap, 500, "too large");
}
