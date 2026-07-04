/* kb_http_accounts.c: console accounts routes (enrollments / revoke / scopes).
 * See kb_http_accounts.h. All routes are console-admin-ACL'd upstream. */
#include "kb_http_accounts.h"

#include "cJSON.h"
#include "db2/enrollments.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ENROLL_LIST_MAX 200

/* Parse "limit=N" from a query string; clamp to [1, ENROLL_LIST_MAX]. */
static int query_limit(const char *qs, int def)
{
   if (!qs)
      return def;
   const char *p = strstr(qs, "limit=");
   if (!p)
      return def;
   int v = atoi(p + 6);
   if (v <= 0)
      return def;
   if (v > ENROLL_LIST_MAX)
      v = ENROLL_LIST_MAX;
   return v;
}

static cJSON *enrollment_to_json(const db2_enrollment_row_t *r)
{
   cJSON *o = cJSON_CreateObject();
   cJSON_AddNumberToObject(o, "id", (double)r->id);
   cJSON_AddStringToObject(o, "scope", r->scope);
   cJSON_AddStringToObject(o, "fingerprint", r->fingerprint);
   cJSON_AddStringToObject(o, "serial", r->serial);
   cJSON_AddStringToObject(o, "state", r->state);
   cJSON_AddStringToObject(o, "issued_at", r->issued_at);
   cJSON_AddStringToObject(o, "last_seen_at", r->last_seen_at);
   cJSON_AddStringToObject(o, "expires_at", r->expires_at);
   cJSON_AddStringToObject(o, "revoked_at", r->revoked_at);
   cJSON_AddBoolToObject(o, "legacy", r->legacy != 0);
   return o;
}

static int emit(cJSON *root, char *out_buf, int out_cap, int status)
{
   char *s = cJSON_PrintUnformatted(root);
   cJSON_Delete(root);
   if (!s || strlen(s) >= (size_t)out_cap)
   {
      free(s);
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"response too large\"}");
      return 500;
   }
   snprintf(out_buf, (size_t)out_cap, "%s", s);
   free(s);
   return status;
}

/* GET /v1/enrollments — paginated list of issued certs. */
static int list_enrollments(const char *query_string, char *out_buf, int out_cap)
{
   int limit = query_limit(query_string, 50);
   db2_enrollment_row_t rows[ENROLL_LIST_MAX];
   int n = db2_enrollment_list(limit, rows, ENROLL_LIST_MAX);
   if (n < 0)
   {
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"enrollments unavailable\"}");
      return 503;
   }
   cJSON *root = cJSON_CreateObject();
   cJSON *arr = cJSON_AddArrayToObject(root, "enrollments");
   for (int i = 0; i < n; i++)
      cJSON_AddItemToArray(arr, enrollment_to_json(&rows[i]));
   cJSON_AddNumberToObject(root, "count", n);
   return emit(root, out_buf, out_cap, 200);
}

/* POST /v1/enrollments/{id}/revoke */
static int revoke_enrollment(const char *path, char *out_buf, int out_cap)
{
   long long id = 0;
   char tail[8] = "";
   /* Exact shape: /v1/enrollments/<id>/revoke (tail guards against extra input). */
   if (sscanf(path, "/v1/enrollments/%lld/revoke%7s", &id, tail) < 1 || tail[0] || id <= 0)
   {
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"bad enrollment id\"}");
      return 400;
   }
   db2_enrollment_row_t row;
   int rc = db2_enrollment_revoke((int64_t)id, &row);
   if (rc == 1)
   {
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"enrollment not found\"}");
      return 404;
   }
   if (rc != 0)
   {
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"revoke failed\"}");
      return 503;
   }
   /* Console-admin action audit (single 0600 audit.log sink). The console also
    * records the human principal on its side (S0); here the kb records the acted
    * credential + target so the revoke is attributable server-side too. */
   audit_log("console_enrollment_revoke", "id=%lld fingerprint=%s scope=%s", id, row.fingerprint,
             row.scope);
   cJSON *root = cJSON_CreateObject();
   cJSON_AddBoolToObject(root, "revoked", 1);
   cJSON_AddItemToObject(root, "enrollment", enrollment_to_json(&row));
   return emit(root, out_buf, out_cap, 200);
}

/* GET /v1/scopes — the scope lattice: distinct scopes with cert counts. */
static int list_scopes(char *out_buf, int out_cap)
{
   db2_enrollment_row_t rows[ENROLL_LIST_MAX];
   int n = db2_enrollment_list(ENROLL_LIST_MAX, rows, ENROLL_LIST_MAX);
   if (n < 0)
   {
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"scopes unavailable\"}");
      return 503;
   }
   /* Aggregate by scope in C (enrollment counts are small). */
   cJSON *root = cJSON_CreateObject();
   cJSON *arr = cJSON_AddArrayToObject(root, "scopes");
   for (int i = 0; i < n; i++)
   {
      const char *scope = rows[i].scope[0] ? rows[i].scope : "(owner)";
      cJSON *found = NULL, *it = NULL;
      cJSON_ArrayForEach(it, arr)
      {
         const cJSON *s = cJSON_GetObjectItem(it, "scope");
         if (s && s->valuestring && strcmp(s->valuestring, scope) == 0)
         {
            found = it;
            break;
         }
      }
      if (found)
      {
         cJSON *c = cJSON_GetObjectItem(found, "count");
         cJSON *a = cJSON_GetObjectItem(found, "active");
         cJSON_SetNumberValue(c, c->valuedouble + 1);
         if (strcmp(rows[i].state, "active") == 0)
            cJSON_SetNumberValue(a, a->valuedouble + 1);
      }
      else
      {
         cJSON *e = cJSON_CreateObject();
         cJSON_AddStringToObject(e, "scope", scope);
         cJSON_AddNumberToObject(e, "count", 1);
         cJSON_AddNumberToObject(e, "active", strcmp(rows[i].state, "active") == 0 ? 1 : 0);
         cJSON_AddItemToArray(arr, e);
      }
   }
   return emit(root, out_buf, out_cap, 200);
}

int kb_http_accounts_route(const char *method, const char *path, const char *query_string,
                           char *out_buf, int out_cap)
{
   if (strcmp(path, "/v1/enrollments") == 0 || strcmp(path, "/v1/enrollments/") == 0)
   {
      if (strcmp(method, "GET") != 0)
      {
         snprintf(out_buf, (size_t)out_cap, "{\"error\":\"method not allowed\"}");
         return 405;
      }
      return list_enrollments(query_string, out_buf, out_cap);
   }
   if (strncmp(path, "/v1/enrollments/", 16) == 0 && strstr(path, "/revoke"))
   {
      if (strcmp(method, "POST") != 0)
      {
         snprintf(out_buf, (size_t)out_cap, "{\"error\":\"method not allowed\"}");
         return 405;
      }
      return revoke_enrollment(path, out_buf, out_cap);
   }
   if (strcmp(path, "/v1/scopes") == 0)
   {
      if (strcmp(method, "GET") != 0)
      {
         snprintf(out_buf, (size_t)out_cap, "{\"error\":\"method not allowed\"}");
         return 405;
      }
      return list_scopes(out_buf, out_cap);
   }
   return -1; /* not an accounts route */
}
