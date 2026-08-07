/* kb_http_code_lifecycle.c -- project lifecycle and repo-trust HTTP handlers.
 *
 * Split out of kb_http_code.c, which crossed the 2500-line hard limit that
 * line-check enforces. These four functions are the only ones in that file
 * concerned with a project's LIFECYCLE (create/promote/retire a generation) and
 * with cross-repo TRUST, rather than with querying an already-indexed project,
 * so they are the natural seam -- the same one kb_http_code_context.c and
 * kb_http_code_graphfb.c were cut along.
 *
 * No behaviour changes: the functions, their signatures and their order are
 * unchanged, and code_method_not_allowed / code_project_manifest_response keep
 * the same callers. */
#include "kb_http_code.h"

#include "aimee.h"
#include "cJSON.h"
#include "db2/code_project_lifecycle.h"
#include "db2/cross_repo_stats.h" /* db2_cross_repo_set_trust, recompute_blocked_symbols */
#include "db2/cross_repo_classify.h"
#include "db2/lifecycle.h" /* db2_is_initialized */
#include "kb_reqctx.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int code_project_manifest_response(const char *operation, const code_project_manifest_t *m,
                                          char *out_buf, int out_cap)
{
   cJSON *resp = cJSON_CreateObject();
   if (!resp)
      return code_scan_write_error(out_buf, out_cap, "out of memory");
   cJSON_AddStringToObject(resp, "status", "ok");
   cJSON_AddStringToObject(resp, "operation", operation);
   cJSON_AddStringToObject(resp, "project", m->project);
   cJSON_AddNumberToObject(resp, "generation", (double)m->generation);
   cJSON_AddStringToObject(resp, "mode", m->mode);
   if (m->criteria[0])
      cJSON_AddStringToObject(resp, "criteria", m->criteria);
   cJSON_AddStringToObject(resp, "manifest_hash", m->manifest_hash);
   cJSON_AddNumberToObject(resp, "total_rows", (double)m->total_rows);
   cJSON *counts = cJSON_AddObjectToObject(resp, "counts");
   for (int i = 0; counts && i < m->target_count; i++)
      cJSON_AddNumberToObject(counts, m->targets[i].table, (double)m->targets[i].rows);
   cJSON *fingerprints = cJSON_AddObjectToObject(resp, "target_fingerprints");
   for (int i = 0; fingerprints && i < m->target_count; i++)
      cJSON_AddStringToObject(fingerprints, m->targets[i].table, m->targets[i].fingerprint);
   char *json = cJSON_PrintUnformatted(resp);
   snprintf(out_buf, (size_t)out_cap, "%s", json ? json : "{\"error\":\"out of memory\"}");
   free(json);
   cJSON_Delete(resp);
   return 200;
}

int handle_post_code_project_lifecycle_route(const char *method, const char *operation,
                                             const char *body, char *out_buf, int out_cap,
                                             int owner)
{
   if (strcmp(method, "POST") != 0)
      return code_method_not_allowed(out_buf, out_cap);
   if (!owner)
   {
      snprintf(out_buf, (size_t)out_cap,
               "{\"error\":\"forbidden: index lifecycle requires the owner credential\"}");
      return 403;
   }
   cJSON *root = cJSON_Parse(body ? body : "{}");
   if (!root)
      return code_scan_write_error(out_buf, out_cap, "invalid json");
   const cJSON *jp = cJSON_GetObjectItemCaseSensitive(root, "project");
   const cJSON *jh = cJSON_GetObjectItemCaseSensitive(root, "confirm_hash");
   const cJSON *jr = cJSON_GetObjectItemCaseSensitive(root, "reason");
   const cJSON *jd = cJSON_GetObjectItemCaseSensitive(root, "retention_days");
   const char *project = cJSON_IsString(jp) ? jp->valuestring : "";
   const char *hash = cJSON_IsString(jh) ? jh->valuestring : "";
   const char *reason = cJSON_IsString(jr) ? jr->valuestring : "";
   int retention_days = cJSON_IsNumber(jd) ? jd->valueint : 30;
   if (!project[0])
   {
      cJSON_Delete(root);
      return code_scan_write_error(out_buf, out_cap, "missing project");
   }
   if (strlen(project) >= sizeof(((code_project_manifest_t *)0)->project))
   {
      cJSON_Delete(root);
      return code_scan_write_error(out_buf, out_cap, "project must be at most 255 characters");
   }
   if (retention_days < 0 || retention_days > 3650)
   {
      cJSON_Delete(root);
      return code_scan_write_error(out_buf, out_cap, "retention_days must be 0..3650");
   }
   if (reason[0] && strlen(reason) > 512)
   {
      cJSON_Delete(root);
      return code_scan_write_error(out_buf, out_cap, "reason must be at most 512 characters");
   }

   /* Audit attribution is derived only from the authenticated request context.
    * Never accept a body-supplied principal: it is an assertion by the caller,
    * not verified identity.  The owner gate above deliberately requires a real
    * actor even for dry runs so auth-off deployments cannot anonymously operate
    * lifecycle controls. */
   char principal[576] = "";
   const kb_principal_t *actor = kb_reqctx_actor();
   if (!actor || kb_identity_key(actor, principal, sizeof(principal)) != 0)
   {
      cJSON_Delete(root);
      snprintf(out_buf, (size_t)out_cap,
               "{\"error\":\"forbidden: verified lifecycle principal required\"}");
      return 403;
   }

   int rc;
   code_project_manifest_t manifest;
   memset(&manifest, 0, sizeof(manifest));
   if (strcmp(operation, "detach") == 0)
   {
      int64_t generation = 0;
      rc = db2_code_project_detach(project, principal, &generation);
      if (rc == 0)
      {
         cJSON_Delete(root);
         cJSON *resp = cJSON_CreateObject();
         if (!resp)
            return code_scan_write_error(out_buf, out_cap, "out of memory");
         cJSON_AddStringToObject(resp, "status", "ok");
         cJSON_AddStringToObject(resp, "operation", "detach");
         cJSON_AddStringToObject(resp, "project", project);
         cJSON_AddNumberToObject(resp, "generation", (double)generation);
         cJSON_AddStringToObject(resp, "state", "detached");
         char *json = cJSON_PrintUnformatted(resp);
         snprintf(out_buf, (size_t)out_cap, "%s", json ? json : "{\"error\":\"out of memory\"}");
         free(json);
         cJSON_Delete(resp);
         return 200;
      }
   }
   else if (strcmp(operation, "purge") == 0)
   {
      if (hash[0] && !reason[0])
      {
         cJSON_Delete(root);
         return code_scan_write_error(out_buf, out_cap, "confirmed purge requires reason");
      }
      rc = hash[0] ? db2_code_project_purge_confirm(project, hash, principal, reason, &manifest)
                   : db2_code_project_purge_manifest(project, &manifest);
      if (rc == 0)
      {
         cJSON_Delete(root);
         return code_project_manifest_response(operation, &manifest, out_buf, out_cap);
      }
   }
   else if (strcmp(operation, "gc") == 0)
   {
      if (hash[0] && !reason[0])
      {
         cJSON_Delete(root);
         return code_scan_write_error(out_buf, out_cap, "confirmed gc requires reason");
      }
      rc = hash[0] ? db2_code_project_gc_confirm(project, retention_days, hash, principal, reason,
                                                 &manifest)
                   : db2_code_project_gc_manifest(project, retention_days, &manifest);
      if (rc == 0)
      {
         cJSON_Delete(root);
         return code_project_manifest_response(operation, &manifest, out_buf, out_cap);
      }
   }
   else
   {
      cJSON_Delete(root);
      return code_scan_write_error(out_buf, out_cap, "unknown lifecycle operation");
   }
   cJSON_Delete(root);
   if (rc == CODE_PROJECT_LIFECYCLE_NOT_FOUND)
   {
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"project not found\"}");
      return 404;
   }
   if (rc == CODE_PROJECT_LIFECYCLE_HASH_MISMATCH)
   {
      snprintf(
          out_buf, (size_t)out_cap,
          "{\"error\":\"manifest changed; run dry-run again\",\"code\":\"manifest_mismatch\"}");
      return 409;
   }
   if (rc == CODE_PROJECT_LIFECYCLE_AUDIT_FAILED)
   {
      snprintf(out_buf, (size_t)out_cap,
               "{\"error\":\"audit commit failed; mutation refused\",\"code\":\"audit_failed\"}");
      return 503;
   }
   snprintf(out_buf, (size_t)out_cap, "{\"error\":\"index lifecycle operation failed\"}");
   return 503;
}

/* S7: POST /v1/code/repo-trust {project, trust:"trusted"|"untrusted", actor?,
 * request_id?}. `owner` (caller holds the unscoped owner credential) is required:
 * a scoped token must not be able to flip trust. Applies the audited db2 trust
 * write and, on a real transition, recomputes the blocked_symbols frequency model
 * (best-effort — the trust write has already committed and bumped trust_epoch,
 * which invalidates cached results either way). */
int handle_post_code_repo_trust(const char *body, char *out_buf, int out_cap, int owner)
{
   if (!owner)
   {
      snprintf(out_buf, (size_t)out_cap,
               "{\"error\":\"forbidden: repo trust requires the owner credential\"}");
      return 403;
   }
   cJSON *root = cJSON_Parse(body ? body : "{}");
   if (!root)
      return code_scan_write_error(out_buf, out_cap, "invalid json");

   char project[256] = "", trust[16] = "", actor[256] = "", request_id[128] = "";
   cJSON *j;
   if ((j = cJSON_GetObjectItemCaseSensitive(root, "project")) && cJSON_IsString(j))
      snprintf(project, sizeof(project), "%s", j->valuestring);
   if ((j = cJSON_GetObjectItemCaseSensitive(root, "trust")) && cJSON_IsString(j))
      snprintf(trust, sizeof(trust), "%s", j->valuestring);
   if ((j = cJSON_GetObjectItemCaseSensitive(root, "actor")) && cJSON_IsString(j))
      snprintf(actor, sizeof(actor), "%s", j->valuestring);
   if ((j = cJSON_GetObjectItemCaseSensitive(root, "request_id")) && cJSON_IsString(j))
      snprintf(request_id, sizeof(request_id), "%s", j->valuestring);
   cJSON_Delete(root);

   if (!project[0])
   {
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"missing project\"}");
      return 400;
   }
   if (strcmp(trust, "trusted") != 0 && strcmp(trust, "untrusted") != 0)
   {
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"trust must be 'trusted' or 'untrusted'\"}");
      return 400;
   }
   if (!db2_is_initialized())
   {
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"failed to open knowledge service store\"}");
      return 503;
   }

   char prior[16] = "";
   int changed = 0;
   int rc =
       db2_cross_repo_set_trust(project, trust, actor, request_id, prior, sizeof(prior), &changed);
   if (rc == 1)
   {
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"no such project\"}");
      return 404;
   }
   if (rc != 0)
   {
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"trust write failed\"}");
      return 503;
   }

   int recomputed = -1; /* -1 = not attempted (no change); >=0 rows; -1 on a recompute error too */
   if (changed)
   {
      if (config_present())
         recomputed = db2_cross_repo_recompute_blocked_symbols(
             config_kb_curator_cross_repo_k(), config_kb_curator_cross_repo_m(),
             config_kb_curator_cross_repo_len_min());
   }

   /* Build via cJSON so the (owner-supplied) project name is JSON-escaped rather
    * than interpolated raw into a template. */
   cJSON *out = cJSON_CreateObject();
   if (!out)
   {
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"out of memory\"}");
      return 500;
   }
   cJSON_AddStringToObject(out, "status", "ok");
   cJSON_AddStringToObject(out, "project", project);
   cJSON_AddStringToObject(out, "prior_trust", prior);
   cJSON_AddStringToObject(out, "new_trust", trust);
   cJSON_AddBoolToObject(out, "changed", changed ? 1 : 0);
   cJSON_AddNumberToObject(out, "blocked_symbols_recomputed", recomputed);
   char *s = cJSON_PrintUnformatted(out);
   cJSON_Delete(out);
   if (!s)
   {
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"out of memory\"}");
      return 500;
   }
   int over = ((int)strlen(s) >= out_cap);
   if (!over)
      snprintf(out_buf, (size_t)out_cap, "%s", s);
   free(s);
   if (over)
   {
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"response too large\"}");
      return 500;
   }
   return 200;
}

int handle_post_code_repo_trust_route(const char *method, const char *body, char *out_buf,
                                      int out_cap, int owner)
{
   if (strcmp(method, "POST") != 0)
      return code_method_not_allowed(out_buf, out_cap);
   return handle_post_code_repo_trust(body, out_buf, out_cap, owner);
}
