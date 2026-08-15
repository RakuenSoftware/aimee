/* kb_http_models.c: /v1/models routes (P2a org model catalog + entitlement).
 *
 * The authenticated actor comes from kb_reqctx (set by the router after verification);
 * every op runs inside a tenant scope (db2_tenant_scope_begin sets aimee.principal), so
 * the entitled read is actor-bound and the org-admin capability for the /org mutations
 * is enforced at the DB layer (kb_principal_is_admin inside the SECURITY DEFINER
 * catalog functions) — a non-admin write is denied there and surfaces here as 403.
 * Every mutation goes through the audited definer functions (WORM audit atomic with the
 * mutation). Catalog-only: the entitled surface carries NO credential/slot field. */

#include "kb_http_models.h"

#include "cJSON.h"
#include "modules/db2/c/db2_tenant.h"
#include "kb_models_validate.h"
#include "kb_reqctx.h"
#include "org_model_catalog.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int emit(cJSON *root, char *out, int cap, int status)
{
   char *s = cJSON_PrintUnformatted(root);
   cJSON_Delete(root);
   if (!s || (int)strlen(s) >= cap)
   {
      free(s);
      snprintf(out, (size_t)cap, "{\"error\":\"response too large\"}");
      return 500;
   }
   snprintf(out, (size_t)cap, "%s", s);
   free(s);
   return status;
}

static int err(char *out, int cap, int status, const char *msg)
{
   snprintf(out, (size_t)cap, "{\"error\":\"%s\"}", msg);
   return status;
}

/* Map a tenant-scope/db2 return into an HTTP status. */
static int tenant_http_status(int rc)
{
   if (rc == DB2_ERR_TENANT_REQUIRES_PG)
      return 503;
   if (rc == DB2_ERR_TENANT_UNAUTHENTICATED)
      return 401;
   if (rc == DB2_ERR_TENANT_DENIED)
      return 403;
   return 500;
}

/* Open a bootstrap/admin tenant scope for the current actor (team 0 = principal only).
 * Returns 0 on success or writes an HTTP status (>0) into out via *http_out. */
static int begin_actor_scope(char *out, int cap, int *http_out)
{
   const kb_principal_t *actor = kb_reqctx_actor();
   if (!actor)
   {
      *http_out = err(out, cap, 401, "authentication required");
      return -1;
   }
   int rc = db2_tenant_scope_begin(actor, 0);
   if (rc != 0)
   {
      *http_out = err(out, cap, tenant_http_status(rc), "tenant scope failed");
      return -1;
   }
   return 0;
}

/* GET /v1/models/entitled -> the caller's entitled models (actor-bound). */
static int handle_entitled(const char *method, char *out, int cap)
{
   if (strcmp(method, "GET") != 0)
      return err(out, cap, 405, "method not allowed");
   int http = 0;
   if (begin_actor_scope(out, cap, &http) != 0)
      return http;
   db2_model_entitled_row_t rows[256];
   int n = db2_model_entitled_list(rows, (int)(sizeof(rows) / sizeof(rows[0])));
   if (db2_tenant_scope_commit() != 0)
      return err(out, cap, 500, "commit failed");
   if (n < 0)
      return err(out, cap, 500, "list failed");
   cJSON *arr = cJSON_CreateArray();
   for (int i = 0; i < n; ++i)
   {
      cJSON *o = cJSON_CreateObject();
      cJSON_AddStringToObject(o, "model_id", rows[i].model_id);
      cJSON_AddStringToObject(o, "display_name", rows[i].display_name);
      cJSON_AddStringToObject(o, "provider", rows[i].provider);
      cJSON_AddStringToObject(o, "wire", rows[i].wire);
      cJSON_AddStringToObject(o, "endpoint", rows[i].endpoint);
      cJSON_AddItemToArray(arr, o);
   }
   cJSON *root = cJSON_CreateObject();
   cJSON_AddItemToObject(root, "models", arr);
   return emit(root, out, cap, 200);
}

/* POST /v1/models/org/add|set {model_id, provider, wire[, display_name, endpoint,
 * enabled]} -> create/update a catalog entry (admin-gated at the DB layer). */
static int handle_org_upsert(const char *method, const char *body, char *out, int cap)
{
   if (strcmp(method, "POST") != 0)
      return err(out, cap, 405, "method not allowed");
   cJSON *b = body ? cJSON_Parse(body) : NULL;
   cJSON *jmodel = b ? cJSON_GetObjectItemCaseSensitive(b, "model_id") : NULL;
   cJSON *jprov = b ? cJSON_GetObjectItemCaseSensitive(b, "provider") : NULL;
   cJSON *jwire = b ? cJSON_GetObjectItemCaseSensitive(b, "wire") : NULL;
   cJSON *jdisp = b ? cJSON_GetObjectItemCaseSensitive(b, "display_name") : NULL;
   cJSON *jendp = b ? cJSON_GetObjectItemCaseSensitive(b, "endpoint") : NULL;
   cJSON *jen = b ? cJSON_GetObjectItemCaseSensitive(b, "enabled") : NULL;
   if (!cJSON_IsString(jmodel) || !cJSON_IsString(jprov) || !cJSON_IsString(jwire))
   {
      cJSON_Delete(b);
      return err(out, cap, 400, "model_id, provider, wire required");
   }
   /* Reject an over-length endpoint at the boundary (the schema CHECK is <=500) rather
    * than letting the snprintf below silently truncate a too-long value into the buffer. */
   if (cJSON_IsString(jendp) && strlen(jendp->valuestring) > 500)
   {
      cJSON_Delete(b);
      return err(out, cap, 400, "endpoint too long (<=500 chars)");
   }
   char model_id[256], provider[128], wire[32], display_name[256], endpoint[DB2_MODEL_ENDPOINT_CAP];
   snprintf(model_id, sizeof(model_id), "%s", jmodel->valuestring);
   snprintf(provider, sizeof(provider), "%s", jprov->valuestring);
   snprintf(wire, sizeof(wire), "%s", jwire->valuestring);
   snprintf(display_name, sizeof(display_name), "%s",
            cJSON_IsString(jdisp) ? jdisp->valuestring : "");
   snprintf(endpoint, sizeof(endpoint), "%s", cJSON_IsString(jendp) ? jendp->valuestring : "");
   /* enabled defaults true; accept an explicit boolean to disable. */
   int enabled = 1;
   if (cJSON_IsBool(jen))
      enabled = cJSON_IsTrue(jen) ? 1 : 0;
   cJSON_Delete(b);

   if (!kb_models_name_clean(model_id, 200))
      return err(out, cap, 400, "invalid model_id (printable, 1-200 chars)");
   if (!kb_models_name_clean(provider, 100))
      return err(out, cap, 400, "invalid provider (printable, 1-100 chars)");
   if (!kb_models_wire_valid(wire))
      return err(out, cap, 400, "wire must be anthropic|openai|responses|gemini");
   if (!kb_models_endpoint_valid(endpoint, 500))
      return err(out, cap, 400, "invalid endpoint (empty or http(s):// URL, <=500 chars)");

   int http = 0;
   if (begin_actor_scope(out, cap, &http) != 0)
      return http;
   int64_t id = 0;
   int rc =
       db2_model_catalog_upsert(model_id, display_name, provider, wire, endpoint, enabled, &id);
   if (rc != 0)
   {
      db2_tenant_scope_rollback();
      if (rc == DB2_MODEL_ERR_DENIED)
         return err(out, cap, 403, "not authorized to manage the model catalog");
      return err(out, cap, 500, "model catalog update failed");
   }
   if (db2_tenant_scope_commit() != 0)
      return err(out, cap, 500, "commit failed");
   cJSON *o = cJSON_CreateObject();
   cJSON_AddNumberToObject(o, "id", (double)id);
   cJSON_AddStringToObject(o, "model_id", model_id);
   return emit(o, out, cap, 200);
}

/* POST /v1/models/org/remove {model_id} -> remove a catalog entry + its entitlements. */
static int handle_org_remove(const char *method, const char *body, char *out, int cap)
{
   if (strcmp(method, "POST") != 0)
      return err(out, cap, 405, "method not allowed");
   cJSON *b = body ? cJSON_Parse(body) : NULL;
   cJSON *jmodel = b ? cJSON_GetObjectItemCaseSensitive(b, "model_id") : NULL;
   if (!cJSON_IsString(jmodel) || !jmodel->valuestring[0])
   {
      cJSON_Delete(b);
      return err(out, cap, 400, "model_id required");
   }
   char model_id[256];
   snprintf(model_id, sizeof(model_id), "%s", jmodel->valuestring);
   cJSON_Delete(b);
   if (!kb_models_name_clean(model_id, 200))
      return err(out, cap, 400, "invalid model_id");

   int http = 0;
   if (begin_actor_scope(out, cap, &http) != 0)
      return http;
   int64_t removed = 0;
   int rc = db2_model_catalog_remove(model_id, &removed);
   if (rc != 0)
   {
      db2_tenant_scope_rollback();
      if (rc == DB2_MODEL_ERR_DENIED)
         return err(out, cap, 403, "not authorized to manage the model catalog");
      return err(out, cap, 500, "model catalog remove failed");
   }
   if (db2_tenant_scope_commit() != 0)
      return err(out, cap, 500, "commit failed");
   cJSON *o = cJSON_CreateObject();
   cJSON_AddBoolToObject(o, "ok", 1);
   cJSON_AddNumberToObject(o, "removed", (double)removed);
   return emit(o, out, cap, 200);
}

/* POST /v1/models/org/entitle|unentitle {model_id, team} -> grant/revoke (model, team). */
static int handle_org_entitle(const char *method, const char *body, int grant, char *out, int cap)
{
   if (strcmp(method, "POST") != 0)
      return err(out, cap, 405, "method not allowed");
   cJSON *b = body ? cJSON_Parse(body) : NULL;
   cJSON *jmodel = b ? cJSON_GetObjectItemCaseSensitive(b, "model_id") : NULL;
   cJSON *jteam = b ? cJSON_GetObjectItemCaseSensitive(b, "team") : NULL;
   if (!cJSON_IsString(jmodel) || !jmodel->valuestring[0] || !cJSON_IsNumber(jteam))
   {
      cJSON_Delete(b);
      return err(out, cap, 400, "model_id (string) and team (number) required");
   }
   char model_id[256];
   snprintf(model_id, sizeof(model_id), "%s", jmodel->valuestring);
   double team_d = jteam->valuedouble;
   cJSON_Delete(b);
   if (!kb_models_name_clean(model_id, 200))
      return err(out, cap, 400, "invalid model_id");
   /* team must be a positive integer within int64 range: reject NaN, a fractional value
    * (e.g. 1.9), <=0, or an out-of-range magnitude BEFORE the double->int64 cast (casting
    * an out-of-range double to int64 is undefined). 9223372036854775808.0 == 2^63 is the
    * first magnitude past INT64_MAX and is exactly representable as a double. */
   if (!(team_d >= 1.0) || team_d >= 9223372036854775808.0 || team_d != (double)(int64_t)team_d)
      return err(out, cap, 400, "team must be a positive integer");
   int64_t team_id = (int64_t)team_d;

   int http = 0;
   if (begin_actor_scope(out, cap, &http) != 0)
      return http;
   int64_t v = 0;
   int rc = grant ? db2_model_entitle(model_id, team_id, &v)
                  : db2_model_unentitle(model_id, team_id, &v);
   if (rc != 0)
   {
      db2_tenant_scope_rollback();
      if (rc == DB2_MODEL_ERR_DENIED)
         return err(out, cap, 403, "not authorized to manage entitlements");
      return err(out, cap, 500, "entitlement update failed (or unknown model/team)");
   }
   if (db2_tenant_scope_commit() != 0)
      return err(out, cap, 500, "commit failed");
   cJSON *o = cJSON_CreateObject();
   cJSON_AddBoolToObject(o, "ok", 1);
   if (grant)
      cJSON_AddNumberToObject(o, "id", (double)v);
   else
      cJSON_AddNumberToObject(o, "removed", (double)v);
   return emit(o, out, cap, 200);
}

int kb_http_models_route(const char *method, const char *path, const char *body, char *out_buf,
                         int out_cap)
{
   if (!path)
      return -1;
   if (strcmp(path, "/v1/models/entitled") == 0)
      return handle_entitled(method, out_buf, out_cap);
   if (strcmp(path, "/v1/models/org/add") == 0 || strcmp(path, "/v1/models/org/set") == 0)
      return handle_org_upsert(method, body, out_buf, out_cap);
   if (strcmp(path, "/v1/models/org/remove") == 0)
      return handle_org_remove(method, body, out_buf, out_cap);
   if (strcmp(path, "/v1/models/org/entitle") == 0)
      return handle_org_entitle(method, body, 1, out_buf, out_cap);
   if (strcmp(path, "/v1/models/org/unentitle") == 0)
      return handle_org_entitle(method, body, 0, out_buf, out_cap);
   return -1; /* not ours */
}
