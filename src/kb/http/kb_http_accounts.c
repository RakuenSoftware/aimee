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

/* Split a comma-separated value list into a JSON array. */
static cJSON *csv_to_array(const char *csv)
{
   cJSON *arr = cJSON_CreateArray();
   const char *p = csv;
   while (p && *p)
   {
      while (*p == ' ' || *p == ',')
         p++;
      const char *start = p;
      while (*p && *p != ',')
         p++;
      size_t n = (size_t)(p - start);
      while (n > 0 && start[n - 1] == ' ')
         n--;
      if (n > 0)
      {
         char item[256];
         if (n >= sizeof(item))
            n = sizeof(item) - 1;
         memcpy(item, start, n);
         item[n] = '\0';
         cJSON_AddItemToArray(arr, cJSON_CreateString(item));
      }
   }
   return arr;
}

static int oidc_config_to_json(const db2_console_oidc_t *c, char *out_buf, int out_cap, int status)
{
   cJSON *root = cJSON_CreateObject();
   cJSON_AddStringToObject(root, "issuer", c->issuer);
   cJSON_AddStringToObject(root, "audience", c->audience);
   cJSON_AddStringToObject(root, "jwks_url", c->jwks_url);
   cJSON_AddStringToObject(root, "admin_claim", c->admin_claim);
   cJSON_AddItemToObject(root, "admin_values", csv_to_array(c->admin_values));
   cJSON_AddStringToObject(root, "updated_at", c->updated_at);
   cJSON_AddBoolToObject(root, "configured",
                         c->issuer[0] && c->audience[0] && c->jwks_url[0] && c->admin_claim[0] &&
                             c->admin_values[0]);
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

/* GET /v1/config/oidc — the console OIDC login config (empty if unset). */
static int get_oidc_config(char *out_buf, int out_cap)
{
   db2_console_oidc_t c;
   int rc = db2_console_oidc_get(&c);
   if (rc < 0)
   {
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"config store unavailable\"}");
      return 503;
   }
   return oidc_config_to_json(&c, out_buf, out_cap, 200); /* rc==1 => zeroed = unset */
}

/* PUT /v1/config/oidc — store the console OIDC config after structural
 * validation. (The console's own verifier fetches + validates the JWKS at login;
 * break-glass recovers a bad config. Deeper server-side JWKS-fetch/SSRF
 * validation here is a documented follow-up.) */
static int put_oidc_config(const char *body, char *out_buf, int out_cap)
{
   cJSON *req = body ? cJSON_Parse(body) : NULL;
   if (!req)
   {
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"bad request: JSON body required\"}");
      return 400;
   }
   const cJSON *iss = cJSON_GetObjectItemCaseSensitive(req, "issuer");
   const cJSON *aud = cJSON_GetObjectItemCaseSensitive(req, "audience");
   const cJSON *jwks = cJSON_GetObjectItemCaseSensitive(req, "jwks_url");
   const cJSON *claim = cJSON_GetObjectItemCaseSensitive(req, "admin_claim");
   const cJSON *vals = cJSON_GetObjectItemCaseSensitive(req, "admin_values");

   const char *jwks_s = cJSON_IsString(jwks) ? jwks->valuestring : "";
   const char *iss_s = cJSON_IsString(iss) ? iss->valuestring : "";
   const char *aud_s = cJSON_IsString(aud) ? aud->valuestring : "";
   if (!iss_s[0] || !aud_s[0] || !cJSON_IsString(claim) || !claim->valuestring[0] || !jwks_s[0] ||
       !cJSON_IsArray(vals) || cJSON_GetArraySize(vals) == 0)
   {
      cJSON_Delete(req);
      snprintf(out_buf, (size_t)out_cap,
               "{\"error\":\"bad request: issuer, audience, jwks_url, admin_claim, and a non-empty "
               "admin_values array are required\"}");
      return 400;
   }
   /* issuer + jwks_url must be https (OIDC IdPs are https endpoints). */
   if (strncmp(iss_s, "https://", 8) != 0 || strncmp(jwks_s, "https://", 8) != 0)
   {
      cJSON_Delete(req);
      snprintf(out_buf, (size_t)out_cap,
               "{\"error\":\"bad request: issuer and jwks_url must be https\"}");
      return 400;
   }
   /* Every admin_values element must be a non-empty string with no comma (the
    * store is comma-separated, so a comma would corrupt the round-trip). */
   {
      const cJSON *vv = NULL;
      cJSON_ArrayForEach(vv, vals)
      {
         if (!cJSON_IsString(vv) || !vv->valuestring[0] || strchr(vv->valuestring, ','))
         {
            cJSON_Delete(req);
            snprintf(out_buf, (size_t)out_cap,
                     "{\"error\":\"bad request: each admin_values entry must be a non-empty string "
                     "without commas\"}");
            return 400;
         }
      }
   }

   db2_console_oidc_t c;
   memset(&c, 0, sizeof(c));
   snprintf(c.issuer, sizeof(c.issuer), "%s", iss_s);
   snprintf(c.audience, sizeof(c.audience), "%s", aud_s);
   snprintf(c.jwks_url, sizeof(c.jwks_url), "%s", jwks_s);
   snprintf(c.admin_claim, sizeof(c.admin_claim), "%s", claim->valuestring);
   /* Join admin_values into the comma-separated store form. */
   size_t vpos = 0;
   const cJSON *v = NULL;
   cJSON_ArrayForEach(v, vals)
   {
      if (!cJSON_IsString(v))
         continue;
      int wrote = snprintf(c.admin_values + vpos, sizeof(c.admin_values) - vpos, "%s%s",
                           vpos ? "," : "", v->valuestring);
      if (wrote > 0 && (size_t)wrote < sizeof(c.admin_values) - vpos)
         vpos += (size_t)wrote;
   }
   cJSON_Delete(req);

   if (db2_console_oidc_put(&c) != 0)
   {
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"config store unavailable\"}");
      return 503;
   }
   audit_log("console_oidc_config_put", "issuer=%s jwks_url=%s", c.issuer, c.jwks_url);
   /* Re-read to return the canonical stored form (with updated_at). */
   db2_console_oidc_t stored;
   if (db2_console_oidc_get(&stored) == 0)
      return oidc_config_to_json(&stored, out_buf, out_cap, 200);
   return oidc_config_to_json(&c, out_buf, out_cap, 200);
}

int kb_http_accounts_route(const char *method, const char *path, const char *query_string,
                           const char *body, char *out_buf, int out_cap)
{
   if (strcmp(path, "/v1/config/oidc") == 0)
   {
      if (strcmp(method, "GET") == 0)
         return get_oidc_config(out_buf, out_cap);
      if (strcmp(method, "PUT") == 0)
         return put_oidc_config(body, out_buf, out_cap);
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"method not allowed\"}");
      return 405;
   }
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
