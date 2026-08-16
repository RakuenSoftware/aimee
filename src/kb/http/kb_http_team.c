/* kb_http_team.c: /v1/team and /v1/project routes (P1 slice 4).
 *
 * Team/project/membership CRUD on aimee-kb. The authenticated actor comes from
 * kb_reqctx (set by the router after verification); every mutating op runs inside a
 * tenant scope (db2_tenant_scope_begin sets aimee.principal + bootstrap_owner), so
 * the org-admin capability is enforced at the DB layer by the RLS write policies —
 * a non-admin write is denied there and surfaces here as 403. Reads are RLS-scoped
 * to the caller's teams. */

#include "kb_http_team.h"

#include "admin_grant.h"
#include "cJSON.h"
#include "modules/db2/c/db2_tenant.h"
#include "kb_reqctx.h"
#include "membership.h"
#include "project.h"
#include "team.h"

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
   char b[256];
   snprintf(b, sizeof(b), "{\"error\":\"%s\"}", msg);
   snprintf(out, (size_t)cap, "%s", b);
   return status;
}

/* A team/project name is printable text of bounded length: reject control chars,
 * DEL, and over-length so a name can't smuggle control bytes into stored state. */
static int name_is_clean(const char *s)
{
   size_t n = strlen(s);
   if (n == 0 || n > 100)
      return 0;
   for (size_t i = 0; i < n; ++i)
      if ((unsigned char)s[i] < 0x20 || (unsigned char)s[i] == 0x7f)
         return 0;
   return 1;
}

/* Map a tenant-scope/db2 return into an HTTP status. */
static int tenant_http_status(int rc)
{
   if (rc == DB2_ERR_TENANT_REQUIRES_PG)
      return 503; /* tenancy needs the Postgres tier */
   if (rc == DB2_ERR_TENANT_UNAUTHENTICATED)
      return 401;
   if (rc == DB2_ERR_TENANT_DENIED)
      return 403;
   return 500;
}

/* Open a bootstrap/admin tenant scope for the current actor (team 0 = principal
 * only). Returns 0 on success or an HTTP status (>0) written into out. */
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

static cJSON *team_json(const db2_team_row_t *t)
{
   cJSON *o = cJSON_CreateObject();
   cJSON_AddNumberToObject(o, "id", (double)t->id);
   cJSON_AddStringToObject(o, "name", t->name);
   cJSON_AddStringToObject(o, "created_at", t->created_at);
   return o;
}

static int handle_team(const char *method, const char *body, char *out, int cap)
{
   int http = 0;
   /* POST /v1/team {name} -> create (admin-gated at the DB layer). */
   if (strcmp(method, "POST") == 0)
   {
      cJSON *b = body ? cJSON_Parse(body) : NULL;
      cJSON *name = b ? cJSON_GetObjectItemCaseSensitive(b, "name") : NULL;
      if (!cJSON_IsString(name) || !name->valuestring[0])
      {
         cJSON_Delete(b);
         return err(out, cap, 400, "name required");
      }
      char nm[128];
      snprintf(nm, sizeof(nm), "%s", name->valuestring);
      cJSON_Delete(b);
      if (!name_is_clean(nm))
         return err(out, cap, 400, "invalid team name (printable, 1-100 chars)");
      if (begin_actor_scope(out, cap, &http) != 0)
         return http;
      int64_t id = 0;
      int rc = db2_team_create(nm, "", &id);
      if (rc != 0)
      {
         db2_tenant_scope_rollback();
         return err(out, cap, 403, "not authorized to create a team");
      }
      if (db2_tenant_scope_commit() != 0)
         return err(out, cap, 500, "commit failed");
      cJSON *o = cJSON_CreateObject();
      cJSON_AddNumberToObject(o, "id", (double)id);
      cJSON_AddStringToObject(o, "name", nm);
      return emit(o, out, cap, 201);
   }
   /* GET /v1/team -> list the caller's visible teams. */
   if (strcmp(method, "GET") == 0)
   {
      if (begin_actor_scope(out, cap, &http) != 0)
         return http;
      db2_team_row_t rows[128];
      int n = db2_team_list(rows, (int)(sizeof(rows) / sizeof(rows[0])));
      if (db2_tenant_scope_commit() != 0)
         return err(out, cap, 500, "commit failed");
      if (n < 0)
         return err(out, cap, 500, "list failed");
      cJSON *arr = cJSON_CreateArray();
      for (int i = 0; i < n; ++i)
         cJSON_AddItemToArray(arr, team_json(&rows[i]));
      cJSON *o = cJSON_CreateObject();
      cJSON_AddItemToObject(o, "teams", arr);
      return emit(o, out, cap, 200);
   }
   return err(out, cap, 405, "method not allowed");
}

static int handle_member(const char *method, const char *body, char *out, int cap)
{
   /* POST /v1/team/member {team, identity_key[, default]} -> add.
    * DELETE /v1/team/member {team, identity_key} -> remove. Both admin-gated. */
   cJSON *b = body ? cJSON_Parse(body) : NULL;
   cJSON *team = b ? cJSON_GetObjectItemCaseSensitive(b, "team") : NULL;
   cJSON *idk = b ? cJSON_GetObjectItemCaseSensitive(b, "identity_key") : NULL;
   if (!cJSON_IsNumber(team) || !cJSON_IsString(idk) || !idk->valuestring[0])
   {
      cJSON_Delete(b);
      return err(out, cap, 400, "team (number) and identity_key required");
   }
   int64_t team_id = (int64_t)team->valuedouble;
   /* identity_key may be up to 600 chars (kb_team_membership CHECK); reject an
    * over-long key rather than truncate it to a DIFFERENT principal. */
   if (strlen(idk->valuestring) > 600)
   {
      cJSON_Delete(b);
      return err(out, cap, 400, "identity_key too long");
   }
   char key[640];
   snprintf(key, sizeof(key), "%s", idk->valuestring);
   int is_default = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(b, "default")) ? 1 : 0;
   cJSON_Delete(b);

   int http = 0;
   if (begin_actor_scope(out, cap, &http) != 0)
      return http;
   int rc;
   if (strcmp(method, "POST") == 0)
   {
      int64_t id = 0;
      rc = db2_membership_add(key, team_id, is_default, &id);
   }
   else if (strcmp(method, "DELETE") == 0)
   {
      rc = db2_membership_remove(key, team_id);
   }
   else
   {
      db2_tenant_scope_rollback();
      return err(out, cap, 405, "method not allowed");
   }
   if (rc != 0)
   {
      db2_tenant_scope_rollback();
      return err(out, cap, 403, "not authorized to modify membership");
   }
   if (db2_tenant_scope_commit() != 0)
      return err(out, cap, 500, "commit failed");
   cJSON *o = cJSON_CreateObject();
   cJSON_AddBoolToObject(o, "ok", 1);
   return emit(o, out, cap, 200);
}

static int handle_project(const char *method, const char *query_string, const char *body, char *out,
                          int cap)
{
   int http = 0;
   if (strcmp(method, "POST") == 0)
   {
      cJSON *b = body ? cJSON_Parse(body) : NULL;
      cJSON *parent = b ? cJSON_GetObjectItemCaseSensitive(b, "parent") : NULL;
      cJSON *name = b ? cJSON_GetObjectItemCaseSensitive(b, "name") : NULL;
      cJSON *mode = b ? cJSON_GetObjectItemCaseSensitive(b, "access_mode") : NULL;
      if (!cJSON_IsNumber(parent) || !cJSON_IsString(name) || !name->valuestring[0])
      {
         cJSON_Delete(b);
         return err(out, cap, 400, "parent (number) and name required");
      }
      int64_t parent_id = (int64_t)parent->valuedouble;
      char nm[128], am[16];
      snprintf(nm, sizeof(nm), "%s", name->valuestring);
      snprintf(am, sizeof(am), "%s",
               (cJSON_IsString(mode) && mode->valuestring[0]) ? mode->valuestring : "team-open");
      cJSON_Delete(b);
      if (!name_is_clean(nm))
         return err(out, cap, 400, "invalid project name (printable, 1-100 chars)");
      if (strcmp(am, "team-open") != 0 && strcmp(am, "restricted") != 0)
         return err(out, cap, 400, "access_mode must be 'team-open' or 'restricted'");
      if (begin_actor_scope(out, cap, &http) != 0)
         return http;
      int64_t id = 0;
      int rc = db2_project_create(parent_id, nm, am, "", &id);
      if (rc != 0)
      {
         db2_tenant_scope_rollback();
         return err(out, cap, 403, "not authorized to create a project");
      }
      if (db2_tenant_scope_commit() != 0)
         return err(out, cap, 500, "commit failed");
      cJSON *o = cJSON_CreateObject();
      cJSON_AddNumberToObject(o, "id", (double)id);
      cJSON_AddNumberToObject(o, "parent", (double)parent_id);
      cJSON_AddStringToObject(o, "name", nm);
      cJSON_AddStringToObject(o, "access_mode", am);
      return emit(o, out, cap, 201);
   }
   if (strcmp(method, "GET") == 0)
   {
      int64_t parent_id = 0;
      /* Parse ?team=<digits> strictly: only match at a parameter boundary (start or
       * after '&'/'?'), require at least one digit, and reject trailing junk. */
      const char *qs = query_string;
      const char *tq = NULL;
      for (const char *p = qs; p && *p;)
      {
         if ((p == qs || p[-1] == '&' || p[-1] == '?') && strncmp(p, "team=", 5) == 0)
         {
            tq = p + 5;
            break;
         }
         const char *amp = strchr(p, '&');
         p = amp ? amp + 1 : NULL;
      }
      if (tq)
      {
         char *end = NULL;
         long long v = strtoll(tq, &end, 10);
         if (end == tq || (*end != '\0' && *end != '&') || v < 0)
            return err(out, cap, 400, "invalid team parameter");
         parent_id = (int64_t)v;
      }
      if (begin_actor_scope(out, cap, &http) != 0)
         return http;
      db2_project_row_t rows[128];
      int n = db2_project_list(parent_id, rows, (int)(sizeof(rows) / sizeof(rows[0])));
      if (db2_tenant_scope_commit() != 0)
         return err(out, cap, 500, "commit failed");
      if (n < 0)
         return err(out, cap, 500, "list failed");
      cJSON *arr = cJSON_CreateArray();
      for (int i = 0; i < n; ++i)
      {
         cJSON *o = cJSON_CreateObject();
         cJSON_AddNumberToObject(o, "id", (double)rows[i].id);
         cJSON_AddNumberToObject(o, "parent", (double)rows[i].parent);
         cJSON_AddStringToObject(o, "name", rows[i].name);
         cJSON_AddStringToObject(o, "access_mode", rows[i].access_mode);
         cJSON_AddItemToArray(arr, o);
      }
      cJSON *o = cJSON_CreateObject();
      cJSON_AddItemToObject(o, "projects", arr);
      return emit(o, out, cap, 200);
   }
   return err(out, cap, 405, "method not allowed");
}

int kb_http_team_route(const char *method, const char *path, const char *query_string,
                       const char *body, char *out_buf, int out_cap)
{
   if (!path)
      return -1;
   if (strcmp(path, "/v1/team/member") == 0)
      return handle_member(method, body, out_buf, out_cap);
   if (strcmp(path, "/v1/team") == 0)
      return handle_team(method, body, out_buf, out_cap);
   if (strcmp(path, "/v1/project") == 0)
      return handle_project(method, query_string, body, out_buf, out_cap);
   return -1; /* not ours */
}
