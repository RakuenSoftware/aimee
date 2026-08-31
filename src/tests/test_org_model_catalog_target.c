#include "modules/db2/c/db_postgres.h"
#include "modules/db2/c/org_model_catalog.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct aimee_pg_stmt
{
   int step;
};

static struct aimee_pg_stmt g_stmt;
static int g_prepare_ok = 1, g_bind_fail, g_step_mode;
static const char *g_cols[10];

int db2_tenant_require_pg(void)
{
   return 0;
}
void *(db2_conn)(void)
{
   return &g_stmt;
}

/* Real code reaches the pool through the db2_conn() macro, which expands to
 * db2_conn_at(site) so a lazy acquire can be attributed. Route the stub. */
void *db2_conn_at(const char *site)
{
   (void)site;
   return (db2_conn)();
}
aimee_pg_stmt_t *aimee_pg_prepare(void *conn, const char *sql, char *err, size_t errlen)
{
   (void)conn;
   (void)err;
   (void)errlen;
   assert(strstr(sql, "org_catalog_bedrock_target(?1,?2)") != NULL);
   g_stmt.step = 0;
   return g_prepare_ok ? &g_stmt : NULL;
}
int aimee_pg_bind_int64(aimee_pg_stmt_t *stmt, const char *name, int64_t value)
{
   (void)stmt;
   assert(strcmp(name, "?1") == 0 && value == 42);
   return g_bind_fail ? -1 : 0;
}
int aimee_pg_bind_text(aimee_pg_stmt_t *stmt, const char *name, const char *value)
{
   (void)stmt;
   assert(strcmp(name, "?2") == 0 && strcmp(value, "model") == 0);
   return g_bind_fail ? -1 : 0;
}
aimee_pg_step_t aimee_pg_step(aimee_pg_stmt_t *stmt, char *err, size_t errlen)
{
   (void)err;
   (void)errlen;
   if (g_step_mode == 0)
      return AIMEE_PG_DONE;
   if (g_step_mode == 1)
      return stmt->step++ == 0 ? AIMEE_PG_ROW : AIMEE_PG_DONE;
   if (g_step_mode == 2)
      return AIMEE_PG_ERR;
   if (g_step_mode == 4)
      return stmt->step++ == 0 ? AIMEE_PG_ROW : AIMEE_PG_ERR;
   return AIMEE_PG_ROW;
}
void aimee_pg_finalize(aimee_pg_stmt_t *stmt)
{
   (void)stmt;
}
int aimee_pg_column_is_null(aimee_pg_stmt_t *stmt, int col)
{
   (void)stmt;
   return g_cols[col] == NULL;
}
const char *aimee_pg_column_text(aimee_pg_stmt_t *stmt, int col)
{
   (void)stmt;
   return g_cols[col];
}

static db2_bedrock_target_row_t foundation_row(void)
{
   db2_bedrock_target_row_t row = {.model_id = "model",
                                   .bedrock_api = "converse",
                                   .model_family = "anthropic",
                                   .target_type = "foundation",
                                   .partition = "aws",
                                   .account = "",
                                   .invoke_region = "us-west-2",
                                   .regions_json = "[\"us-west-2\"]",
                                   .underlying_json = "[]",
                                   .endpoint = ""};
   return row;
}

static int all_zero(const void *p, size_t n)
{
   const unsigned char *b = p;
   for (size_t i = 0; i < n; i++)
      if (b[i])
         return 0;
   return 1;
}

static void decode_hostile_rows(void)
{
   db2_bedrock_target_t out;
   db2_bedrock_target_row_t row = foundation_row();
   assert(db2_model_bedrock_target_decode_row(&row, &out) == DB2_BEDROCK_TARGET_OK);
   assert(out.n_regions == 1 && strcmp(out.invoke_region, "us-west-2") == 0);

   const char *bad_json[] = {
       "{}", "[1]", "[null]", "[\"us\\u0000east\"]", "[\"us\\u0001east\"]", "[[\"us-east-1\"]]",
       NULL};
   for (int i = 0; bad_json[i]; i++)
   {
      row = foundation_row();
      row.regions_json = bad_json[i];
      memset(&out, 0xa5, sizeof(out));
      assert(db2_model_bedrock_target_decode_row(&row, &out) == DB2_BEDROCK_TARGET_INVALID);
      assert(all_zero(&out, sizeof(out)));
   }

   char many[1024] = "[";
   for (int i = 0; i < DB2_BEDROCK_ARRAY_MAX + 1; i++)
      snprintf(many + strlen(many), sizeof(many) - strlen(many), "%s\"r%d\"", i ? "," : "", i);
   strncat(many, "]", sizeof(many) - strlen(many) - 1);
   row = foundation_row();
   row.regions_json = many;
   assert(db2_model_bedrock_target_decode_row(&row, &out) == DB2_BEDROCK_TARGET_INVALID);

   char oversized[208];
   memset(oversized, 'x', sizeof(oversized) - 1);
   oversized[sizeof(oversized) - 1] = 0;
   row = foundation_row();
   row.model_id = oversized;
   assert(db2_model_bedrock_target_decode_row(&row, &out) == DB2_BEDROCK_TARGET_INVALID);

   row = foundation_row();
   row.invoke_region = NULL;
   memset(&out, 0xa5, sizeof(out));
   assert(db2_model_bedrock_target_decode_row(&row, &out) == DB2_BEDROCK_TARGET_INVALID);
   assert(all_zero(&out, sizeof(out)));

   row = foundation_row();
   row.model_family = "unknown-family";
   assert(db2_model_bedrock_target_decode_row(&row, &out) == DB2_BEDROCK_TARGET_INVALID);

   row = foundation_row();
   row.regions_json = "[\"us-west-2\",\"us-west-2\"]";
   assert(db2_model_bedrock_target_decode_row(&row, &out) == DB2_BEDROCK_TARGET_INVALID);

   char *huge = malloc(DB2_BEDROCK_ARRAY_MAX * DB2_BEDROCK_ARN_CAP + 4096);
   assert(huge != NULL);
   memset(huge, ' ', DB2_BEDROCK_ARRAY_MAX * DB2_BEDROCK_ARN_CAP + 4094);
   huge[DB2_BEDROCK_ARRAY_MAX * DB2_BEDROCK_ARN_CAP + 4094] = 0;
   row = foundation_row();
   row.underlying_json = huge;
   assert(db2_model_bedrock_target_decode_row(&row, &out) == DB2_BEDROCK_TARGET_INVALID);
   free(huge);

   row = foundation_row();
   row.target_type = "application-inference-profile";
   row.account = "123456789012";
   row.invoke_region = "us-west-2";
   row.regions_json = "[\"us-east-1\",\"us-west-2\"]";
   row.underlying_json = "[\"arn:aws:bedrock:us-east-1::foundation-model/anthropic.claude\","
                         "\"arn:aws:bedrock:us-west-2::foundation-model/anthropic.claude\"]";
   assert(db2_model_bedrock_target_decode_row(&row, &out) == DB2_BEDROCK_TARGET_OK);
   row.underlying_json = "[\"arn:aws:bedrock:eu-west-1::foundation-model/anthropic.claude\"]";
   assert(db2_model_bedrock_target_decode_row(&row, &out) == DB2_BEDROCK_TARGET_INVALID);
   row.underlying_json = "[\"arn:aws:bedrock:us-east-1::foundation-model/anthropic.claude\"]";
   assert(db2_model_bedrock_target_decode_row(&row, &out) == DB2_BEDROCK_TARGET_INVALID);
}

static void resolver_result_mapping(void)
{
   db2_bedrock_target_t out;
   db2_bedrock_target_row_t row = foundation_row();
   const char *values[] = {
       row.model_id, row.bedrock_api,   row.model_family, row.target_type,     row.partition,
       row.account,  row.invoke_region, row.regions_json, row.underlying_json, row.endpoint};
   memcpy(g_cols, values, sizeof(values));

   g_prepare_ok = 0;
   assert(db2_model_bedrock_target_resolve(42, "model", &out) == DB2_BEDROCK_TARGET_ERROR);
   g_prepare_ok = 1;
   g_bind_fail = 1;
   assert(db2_model_bedrock_target_resolve(42, "model", &out) == DB2_BEDROCK_TARGET_ERROR);
   g_bind_fail = 0;
   g_step_mode = 0;
   assert(db2_model_bedrock_target_resolve(42, "model", &out) == DB2_BEDROCK_TARGET_UNAVAILABLE);
   g_step_mode = 2;
   assert(db2_model_bedrock_target_resolve(42, "model", &out) == DB2_BEDROCK_TARGET_ERROR);
   g_step_mode = 1;
   assert(db2_model_bedrock_target_resolve(42, "model", &out) == DB2_BEDROCK_TARGET_OK);
   g_cols[0] = "different-model";
   assert(db2_model_bedrock_target_resolve(42, "model", &out) == DB2_BEDROCK_TARGET_INVALID);
   assert(all_zero(&out, sizeof(out)));
   g_cols[0] = row.model_id;
   g_cols[7] = "{}";
   memset(&out, 0xa5, sizeof(out));
   assert(db2_model_bedrock_target_resolve(42, "model", &out) == DB2_BEDROCK_TARGET_INVALID);
   assert(all_zero(&out, sizeof(out)));
   g_cols[7] = row.regions_json;
   g_step_mode = 4;
   assert(db2_model_bedrock_target_resolve(42, "model", &out) == DB2_BEDROCK_TARGET_ERROR);
   assert(all_zero(&out, sizeof(out)));
   g_step_mode = 3;
   assert(db2_model_bedrock_target_resolve(42, "model", &out) == DB2_BEDROCK_TARGET_INVALID);
   assert(all_zero(&out, sizeof(out)));
}

int main(void)
{
   decode_hostile_rows();
   resolver_result_mapping();
   puts("org model bedrock target: hostile-row and typed-result tests passed");
   return 0;
}
