/* db2/model_catalog.c: P2a org model catalog + entitlement — Postgres via libpq.
 * See org_model_catalog.h. Mirrors the db2/team.c access pattern. Every mutation goes
 * through the audited SECURITY DEFINER functions (org_catalog_upsert/_remove,
 * org_model_entitle/_unentitle) rather than a raw INSERT, so the WORM audit append is
 * atomic with the mutation in one transaction. Reads go through org_catalog_entitled(),
 * which is actor-bound to aimee.principal (no confused-deputy). Tenant-scoped: requires
 * the RLS-enforcing Postgres backend. */

#include "org_model_catalog.h"

#include "db2_internal.h"
#include "db2_tenant.h"
#include "db_postgres.h"
#include "kb_models_validate.h"
#include "cJSON.h"

#include <stdio.h>
#include <string.h>

static int bedrock_token(const char *s, size_t cap, int empty_ok)
{
   if (!s)
      return 0;
   size_t n = strlen(s);
   if (n >= cap || (!empty_ok && n == 0))
      return 0;
   for (size_t i = 0; i < n; i++)
   {
      unsigned char c = (unsigned char)s[i];
      if (c < 0x20 || c == 0x7f)
         return 0;
      if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '_' || c == '.' || c == ':' || c == '/' || c == '-'))
         return 0;
   }
   return 1;
}

static int bedrock_copy(char *out, size_t cap, const char *in, int empty_ok)
{
   if (!bedrock_token(in, cap, empty_ok))
      return -1;
   memcpy(out, in, strlen(in) + 1);
   return 0;
}

static int raw_nul_escape(const char *json)
{
   return json && strstr(json, "\\u0000") != NULL;
}

/* PostgreSQL constrains these arrays to 64 elements of at most 255/511 bytes,
 * but the decoder is also an explicit hostile-adapter boundary.  Bound the raw
 * JSON before cJSON allocates a tree so a corrupt/injected row cannot turn into
 * an unbounded allocation. */
#define BEDROCK_REGIONS_JSON_CAP    (DB2_BEDROCK_ARRAY_MAX * (DB2_BEDROCK_REGION_CAP + 3) + 2)
#define BEDROCK_UNDERLYING_JSON_CAP (DB2_BEDROCK_ARRAY_MAX * (DB2_BEDROCK_ARN_CAP + 3) + 2)

static int bedrock_json_array(const char *json, size_t json_cap, char *out, size_t stride,
                              size_t elem_cap, size_t *out_n)
{
   *out_n = 0;
   if (!json || strnlen(json, json_cap) == json_cap || raw_nul_escape(json))
      return -1;
   const char *end = NULL;
   cJSON *root = cJSON_ParseWithOpts(json, &end, 1);
   if (!root || !cJSON_IsArray(root))
   {
      cJSON_Delete(root);
      return -1;
   }
   int n = cJSON_GetArraySize(root);
   if (n < 0 || n > DB2_BEDROCK_ARRAY_MAX)
   {
      cJSON_Delete(root);
      return -1;
   }
   for (int i = 0; i < n; i++)
   {
      cJSON *v = cJSON_GetArrayItem(root, i);
      if (!cJSON_IsString(v) || !v->valuestring ||
          bedrock_copy(out + (size_t)i * stride, elem_cap, v->valuestring, 0) != 0)
      {
         cJSON_Delete(root);
         return -1;
      }
   }
   *out_n = (size_t)n;
   cJSON_Delete(root);
   return 0;
}

static int region_present(const db2_bedrock_target_t *t, const char *region)
{
   for (size_t i = 0; i < t->n_regions; i++)
      if (strcmp(t->regions[i], region) == 0)
         return 1;
   return 0;
}

static int regions_unique(const db2_bedrock_target_t *t)
{
   for (size_t i = 0; i < t->n_regions; i++)
      for (size_t j = i + 1; j < t->n_regions; j++)
         if (strcmp(t->regions[i], t->regions[j]) == 0)
            return 0;
   return 1;
}

static int family_supported(const char *family)
{
   static const char *const families[] = {"anthropic", "amazon-nova", "amazon-titan", "meta-llama",
                                          "mistral",   "cohere",      "ai21"};
   for (size_t i = 0; i < sizeof(families) / sizeof(families[0]); i++)
      if (strcmp(family, families[i]) == 0)
         return 1;
   return 0;
}

static int account_valid(const char *s)
{
   if (!s || strlen(s) != 12)
      return 0;
   for (size_t i = 0; i < 12; i++)
      if (s[i] < '0' || s[i] > '9')
         return 0;
   return 1;
}

static int region_valid(const char *s)
{
   if (!s || !s[0] || strlen(s) > 63)
      return 0;
   for (const unsigned char *p = (const unsigned char *)s; *p; p++)
      if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '-'))
         return 0;
   return 1;
}

static int underlying_region(const char *arn, const char *partition, char region[256])
{
   char prefix[48];
   int n = snprintf(prefix, sizeof(prefix), "arn:%s:bedrock:", partition);
   if (n <= 0 || (size_t)n >= sizeof(prefix) || strncmp(arn, prefix, (size_t)n) != 0)
      return -1;
   const char *start = arn + n;
   const char *suffix = strstr(start, "::foundation-model/");
   const char *resource = suffix ? suffix + strlen("::foundation-model/") : NULL;
   if (!suffix || suffix == start || strchr(start, ':') != suffix || !resource[0])
      return -1;
   size_t rn = (size_t)(suffix - start);
   if (rn >= DB2_BEDROCK_REGION_CAP)
      return -1;
   memcpy(region, start, rn);
   region[rn] = 0;
   return region_valid(region) ? 0 : -1;
}

db2_bedrock_target_result_t db2_model_bedrock_target_decode_row(const db2_bedrock_target_row_t *row,
                                                                db2_bedrock_target_t *out)
{
   if (!out)
      return DB2_BEDROCK_TARGET_INVALID;
   memset(out, 0, sizeof(*out));
   if (!row || bedrock_copy(out->model_id, 201, row->model_id, 0) != 0 ||
       bedrock_copy(out->bedrock_api, sizeof(out->bedrock_api), row->bedrock_api, 0) != 0 ||
       bedrock_copy(out->model_family, sizeof(out->model_family), row->model_family, 0) != 0 ||
       bedrock_copy(out->target_type, sizeof(out->target_type), row->target_type, 0) != 0 ||
       bedrock_copy(out->partition, sizeof(out->partition), row->partition, 0) != 0 ||
       bedrock_copy(out->account, sizeof(out->account), row->account, 1) != 0 ||
       bedrock_copy(out->invoke_region, sizeof(out->invoke_region), row->invoke_region, 0) != 0 ||
       bedrock_copy(out->endpoint, sizeof(out->endpoint), row->endpoint, 1) != 0 ||
       bedrock_json_array(row->regions_json, BEDROCK_REGIONS_JSON_CAP, (char *)out->regions,
                          sizeof(out->regions[0]), sizeof(out->regions[0]), &out->n_regions) != 0 ||
       bedrock_json_array(row->underlying_json, BEDROCK_UNDERLYING_JSON_CAP,
                          (char *)out->underlying_fm_arns, sizeof(out->underlying_fm_arns[0]),
                          sizeof(out->underlying_fm_arns[0]), &out->n_underlying) != 0)
      goto invalid;
   if (strcmp(out->bedrock_api, "converse") != 0 || !family_supported(out->model_family) ||
       out->endpoint[0] ||
       (strcmp(out->partition, "aws") != 0 && strcmp(out->partition, "aws-us-gov") != 0 &&
        strcmp(out->partition, "aws-cn") != 0) ||
       out->n_regions == 0 || !regions_unique(out))
      goto invalid;
   if (!region_valid(out->invoke_region))
      goto invalid;
   for (size_t i = 0; i < out->n_regions; i++)
      if (!region_valid(out->regions[i]))
         goto invalid;
   int profile = strcmp(out->target_type, "application-inference-profile") == 0 ||
                 strcmp(out->target_type, "cross-region-inference-profile") == 0;
   int nonprofile = strcmp(out->target_type, "foundation") == 0 ||
                    strcmp(out->target_type, "provisioned") == 0 ||
                    strcmp(out->target_type, "custom") == 0;
   if (!profile && !nonprofile)
      goto invalid;
   if (nonprofile && (out->n_regions != 1 || strcmp(out->regions[0], out->invoke_region) != 0 ||
                      out->n_underlying != 0))
      goto invalid;
   if (strcmp(out->target_type, "foundation") == 0)
   {
      if (out->account[0])
         goto invalid;
   }
   else if (!account_valid(out->account))
      goto invalid;
   if (profile)
   {
      if (out->n_underlying == 0)
         goto invalid;
      int covered[DB2_BEDROCK_ARRAY_MAX] = {0};
      for (size_t i = 0; i < out->n_underlying; i++)
      {
         char region[DB2_BEDROCK_REGION_CAP];
         if (underlying_region(out->underlying_fm_arns[i], out->partition, region) != 0 ||
             !region_present(out, region))
            goto invalid;
         for (size_t j = 0; j < out->n_regions; j++)
            if (strcmp(out->regions[j], region) == 0)
               covered[j] = 1;
      }
      for (size_t i = 0; i < out->n_regions; i++)
         if (!covered[i])
            goto invalid;
   }
   return DB2_BEDROCK_TARGET_OK;
invalid:
   memset(out, 0, sizeof(*out));
   return DB2_BEDROCK_TARGET_INVALID;
}

db2_bedrock_target_result_t db2_model_bedrock_target_resolve(int64_t team_id, const char *model_id,
                                                             db2_bedrock_target_t *out)
{
   if (!out)
      return DB2_BEDROCK_TARGET_INVALID;
   memset(out, 0, sizeof(*out));
   if (team_id <= 0 || !bedrock_token(model_id, sizeof(out->model_id), 0))
      return DB2_BEDROCK_TARGET_INVALID;
   if (db2_tenant_require_pg() != 0)
      return DB2_BEDROCK_TARGET_ERROR;
   void *conn = db2_conn();
   if (!conn)
      return DB2_BEDROCK_TARGET_ERROR;
   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn,
       "SELECT model_id,bedrock_api,model_family,target_type,partition,account,invoke_region,"
       "region_set_json,underlying_json,endpoint FROM org_catalog_bedrock_target(?1,?2)",
       err, sizeof(err));
   if (!st)
      return DB2_BEDROCK_TARGET_ERROR;
   if (aimee_pg_bind_int64(st, "?1", team_id) != 0 || aimee_pg_bind_text(st, "?2", model_id) != 0)
   {
      aimee_pg_finalize(st);
      return DB2_BEDROCK_TARGET_ERROR;
   }
   aimee_pg_step_t step = aimee_pg_step(st, err, sizeof(err));
   if (step == AIMEE_PG_DONE)
   {
      aimee_pg_finalize(st);
      return DB2_BEDROCK_TARGET_UNAVAILABLE;
   }
   if (step != AIMEE_PG_ROW)
   {
      aimee_pg_finalize(st);
      return DB2_BEDROCK_TARGET_ERROR;
   }
   db2_bedrock_target_row_t row = {0};
   const char **cols[] = {
       &row.model_id, &row.bedrock_api,   &row.model_family, &row.target_type,     &row.partition,
       &row.account,  &row.invoke_region, &row.regions_json, &row.underlying_json, &row.endpoint};
   for (int i = 0; i < 10; i++)
      if (!aimee_pg_column_is_null(st, i))
         *cols[i] = aimee_pg_column_text(st, i);
   db2_bedrock_target_result_t result = db2_model_bedrock_target_decode_row(&row, out);
   if (result == DB2_BEDROCK_TARGET_OK && strcmp(out->model_id, model_id) != 0)
      result = DB2_BEDROCK_TARGET_INVALID;
   step = aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);
   if (step == AIMEE_PG_ERR)
      result = DB2_BEDROCK_TARGET_ERROR;
   else if (step != AIMEE_PG_DONE)
      result = DB2_BEDROCK_TARGET_INVALID;
   if (result != DB2_BEDROCK_TARGET_OK)
      memset(out, 0, sizeof(*out));
   return result;
}

/* An admin-gated definer RAISEs "<fn>: admin only" (SQLSTATE 42501) when the caller is
 * not an org-admin; libpq surfaces that message text in the step error. Map it to the
 * distinct DB2_MODEL_ERR_DENIED so a mutation handler can return 403 vs 500 (every other
 * failure — constraint/unknown model/team/audit — stays a generic -1). */
static int model_step_err(const char *err)
{
   return (err && strstr(err, "admin only")) ? DB2_MODEL_ERR_DENIED : -1;
}

int db2_model_catalog_list(db2_model_catalog_row_t *out, int max)
{
   int g = db2_tenant_require_pg();
   if (g)
      return g;
   if (!out || max <= 0)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;
   char err[256] = "";
   /* Admin-only read (RLS p_catalog_admin_read). The operator CLI runs as an admin
    * principal, so this direct SELECT is admitted; the runtime role has no catalog
    * SELECT at all and must use org_catalog_entitled() instead. */
   aimee_pg_stmt_t *st =
       aimee_pg_prepare(conn,
                        "SELECT model_id, display_name, provider, wire, endpoint, enabled::int"
                        " FROM org_model_catalog ORDER BY model_id",
                        err, sizeof(err));
   if (!st)
      return -1;
   int n = 0;
   while (n < max && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      db2_model_catalog_row_t *r = &out[n++];
      memset(r, 0, sizeof(*r));
      const char *c;
      c = aimee_pg_column_text(st, 0);
      snprintf(r->model_id, sizeof(r->model_id), "%s", c ? c : "");
      c = aimee_pg_column_text(st, 1);
      snprintf(r->display_name, sizeof(r->display_name), "%s", c ? c : "");
      c = aimee_pg_column_text(st, 2);
      snprintf(r->provider, sizeof(r->provider), "%s", c ? c : "");
      c = aimee_pg_column_text(st, 3);
      snprintf(r->wire, sizeof(r->wire), "%s", c ? c : "");
      c = aimee_pg_column_text(st, 4);
      snprintf(r->endpoint, sizeof(r->endpoint), "%s", c ? c : "");
      r->enabled = aimee_pg_column_int64(st, 5) ? 1 : 0;
   }
   aimee_pg_finalize(st);
   return n;
}

int db2_model_entitled_list(db2_model_entitled_row_t *out, int max)
{
   int g = db2_tenant_require_pg();
   if (g)
      return g;
   if (!out || max <= 0)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;
   char err[256] = "";
   /* org_catalog_entitled() reads current_setting('aimee.principal') itself — no arg,
    * so a caller can never nominate another principal's memberships. */
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn, "SELECT model_id, display_name, provider, wire, endpoint FROM org_catalog_entitled()",
       err, sizeof(err));
   if (!st)
      return -1;
   int n = 0;
   while (n < max && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      db2_model_entitled_row_t *r = &out[n++];
      memset(r, 0, sizeof(*r));
      const char *c;
      c = aimee_pg_column_text(st, 0);
      snprintf(r->model_id, sizeof(r->model_id), "%s", c ? c : "");
      c = aimee_pg_column_text(st, 1);
      snprintf(r->display_name, sizeof(r->display_name), "%s", c ? c : "");
      c = aimee_pg_column_text(st, 2);
      snprintf(r->provider, sizeof(r->provider), "%s", c ? c : "");
      c = aimee_pg_column_text(st, 3);
      snprintf(r->wire, sizeof(r->wire), "%s", c ? c : "");
      c = aimee_pg_column_text(st, 4);
      snprintf(r->endpoint, sizeof(r->endpoint), "%s", c ? c : "");
   }
   aimee_pg_finalize(st);
   return n;
}

int db2_model_catalog_upsert(const char *model_id, const char *display_name, const char *provider,
                             const char *wire, const char *endpoint, int enabled, int64_t *out_id)
{
   int g = db2_tenant_require_pg();
   if (g)
      return g;
   if (!model_id || !model_id[0] || !provider || !provider[0] || !wire || !wire[0])
      return -1;
   /* Validate identically to the HTTP route (kb_http_models.c) so the CLI cannot store a
    * control-char/invalid value the HTTP path rejects — this C layer is the single choke
    * point both callers pass through. Rejected here BEFORE any DB round-trip. */
   if (!kb_models_name_clean(model_id, 200))
      return -1;
   if (!kb_models_name_clean(provider, 100))
      return -1;
   if (!kb_models_wire_valid(wire))
      return -1;
   if (!kb_models_endpoint_valid(endpoint ? endpoint : "", 500))
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;
   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, "SELECT org_catalog_upsert(?1, ?2, ?3, ?4, ?5, ?6)",
                                          err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", model_id);
   aimee_pg_bind_text(st, "?2", display_name ? display_name : "");
   aimee_pg_bind_text(st, "?3", provider);
   aimee_pg_bind_text(st, "?4", wire);
   aimee_pg_bind_text(st, "?5", endpoint ? endpoint : "");
   /* The 6th arg is BOOLEAN; bind a text literal ('true'/'false') so libpq's
    * unknown-type param coerces to boolean (there is no implicit integer->boolean
    * cast in a function-argument position). */
   aimee_pg_bind_text(st, "?6", enabled ? "true" : "false");
   aimee_pg_step_t step = aimee_pg_step(st, err, sizeof(err));
   int64_t id = (step == AIMEE_PG_ROW) ? aimee_pg_column_int64(st, 0) : 0;
   aimee_pg_finalize(st);
   if (step != AIMEE_PG_ROW)
      return model_step_err(err);
   if (out_id)
      *out_id = id;
   return 0;
}

int db2_model_catalog_remove(const char *model_id, int64_t *out_removed)
{
   int g = db2_tenant_require_pg();
   if (g)
      return g;
   if (!model_id || !model_id[0])
      return -1;
   if (!kb_models_name_clean(model_id, 200))
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;
   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, "SELECT org_catalog_remove(?1)", err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", model_id);
   aimee_pg_step_t step = aimee_pg_step(st, err, sizeof(err));
   int64_t removed = (step == AIMEE_PG_ROW) ? aimee_pg_column_int64(st, 0) : 0;
   aimee_pg_finalize(st);
   if (step != AIMEE_PG_ROW)
      return model_step_err(err);
   if (out_removed)
      *out_removed = removed;
   return 0;
}

static int model_ent_op(const char *sql, const char *model_id, int64_t team_id, int64_t *out)
{
   int g = db2_tenant_require_pg();
   if (g)
      return g;
   if (!model_id || !model_id[0] || team_id <= 0)
      return -1;
   if (!kb_models_name_clean(model_id, 200))
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;
   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", model_id);
   aimee_pg_bind_int64(st, "?2", team_id);
   aimee_pg_step_t step = aimee_pg_step(st, err, sizeof(err));
   int64_t v = (step == AIMEE_PG_ROW) ? aimee_pg_column_int64(st, 0) : 0;
   aimee_pg_finalize(st);
   if (step != AIMEE_PG_ROW)
      return model_step_err(err);
   if (out)
      *out = v;
   return 0;
}

int db2_model_entitle(const char *model_id, int64_t team_id, int64_t *out_id)
{
   return model_ent_op("SELECT org_model_entitle(?1, ?2)", model_id, team_id, out_id);
}

int db2_model_unentitle(const char *model_id, int64_t team_id, int64_t *out_removed)
{
   return model_ent_op("SELECT org_model_unentitle(?1, ?2)", model_id, team_id, out_removed);
}
