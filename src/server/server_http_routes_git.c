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
#include "cJSON.h"
#include "config.h" /* MAX_PATH_LEN */
#include "log.h"
#include "git_forge_vault.h" /* GIT_FORGE_VAULT_AGENT/SSHKEY_CRED — per-webuser ssh-key vault */
#include "git_host_cred.h"   /* per-host git credential store for /v1/git/credentials */
#include "git_ops.h"         /* git_ops_run for /v1/workspace/git (WP-E) */
#include "git_org_repos.h"   /* git_org_repos_list for /v1/workspace/org-repos */
#include "git_project.h"     /* git_project_clone/_delete for /v1/workspace/clone + delete */
#include "git_ssh_agent.h"   /* git_ssh_agent_stop — drop live key handles on revoke */
#include "index.h"           /* index_scan_project after a webuser clone (WP-D) */
#include "kb_client.h"       /* kb_client_index_scan — push webuser clones into aimee-kb */
#include "vault_service.h"   /* vault_service_set/delete for the per-webuser ssh-key route */
#include "webuser_editor.h"  /* webuser_editor_ensure for /v1/workspace/editor (WP-I) */
#include "workspace_scope.h" /* ws_scope_user_root — project workspace root */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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
 * principal comes from the attested identity (server.token-gated X-Aimee-Webuser),
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

   /* The org: the request's `owner` field — the wizard already knows which
    * org it is bulk-cloning (this was previously parsed and dropped). Every
    * repo in the batch lands under it. */
   const cJSON *jowner = cJSON_GetObjectItemCaseSensitive(body, "owner");
   const char *owner =
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
      /* token=NULL → the host's stored credential (or server identity) is used. */
      int rc = url ? git_project_clone(principal, url, name, owner, NULL, dest, sizeof(dest), pname,
                                       sizeof(pname), err, sizeof(err))
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
         cJSON_AddStringToObject(r, "error", url ? err : "missing clone_url");
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

/* POST /v1/workspace/projects/delete {ref, force?} — delete a cloned project
 * under the calling webchat user's scoped workspace (webchat project lifecycle
 * proposal, slice 2). The ONLY identity source is the attested X-Aimee-Webuser
 * principal; another webuser's ref is a plain 404. The last holder of a ref
 * triggers the fenced kb purge (503 abort without `force`); other holders keep
 * the shared knowledge (kb_status "retained"). Capability: tool:execute. */
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
   const cJSON *jforce = cJSON_GetObjectItemCaseSensitive(body, "force");
   char ref[GIT_PROJECT_NAME_MAX];
   ref[0] = '\0';
   if (cJSON_IsString(jref) && jref->valuestring && strlen(jref->valuestring) < sizeof(ref))
      snprintf(ref, sizeof(ref), "%s", jref->valuestring);
   int force = cJSON_IsBool(jforce) && cJSON_IsTrue(jforce);
   cJSON_Delete(body);
   if (!ref[0])
      return err_json(resp, cap, 400, "ref required");

   git_project_delete_result_t res;
   char err[512];
   int rc = git_project_delete(principal, ref, force, &res, err, sizeof(err));
   if (rc == GP_ERR_NOT_FOUND)
   {
      free(res.kb_detail);
      return err_json(resp, cap, 404, "not found");
   }
   if (rc == GP_ERR_KB_UNAVAILABLE)
   {
      cJSON *out = cJSON_CreateObject();
      cJSON_AddStringToObject(out, "error", err);
      cJSON *kb = res.kb_detail ? cJSON_Parse(res.kb_detail) : NULL;
      if (kb)
         cJSON_AddItemToObject(out, "kb", kb);
      free(res.kb_detail);
      char *s = cJSON_PrintUnformatted(out);
      int n = s ? snprintf(resp, (size_t)cap, "%s", s) : -1;
      free(s);
      cJSON_Delete(out);
      return (n > 0 && n < cap) ? 503 : err_json(resp, cap, 503, "knowledge service unavailable");
   }
   if (rc != 0)
   {
      free(res.kb_detail);
      return err_json(resp, cap, 400, err);
   }

   cJSON *out = cJSON_CreateObject();
   cJSON_AddBoolToObject(out, "ok", 1);
   cJSON_AddStringToObject(out, "ref", ref);
   cJSON_AddStringToObject(out, "kb_status", res.kb_status);
   cJSON_AddStringToObject(out, "purge_id", res.purge_id);
   cJSON *kb = res.kb_detail ? cJSON_Parse(res.kb_detail) : NULL;
   if (kb)
      cJSON_AddItemToObject(out, "kb", kb);
   free(res.kb_detail);
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
