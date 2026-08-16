/* memory_row_mapper_pg.c: DB2 row mappers shared between memory modules.
 * Reads via the aimee_pg_* surface and keeps the 12-column memory projection
 * out of memory_query.c's line budget. */

#include "../headers/aimee.h" /* memory_t */
#include "db2_internal.h"
#include "db_postgres.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

/* 13-col SELECT (id, tier, kind, key, content, confidence, use_count,
 * last_used_at, created_at, updated_at, source_session, salience,
 * provenance_category). */
void db2_fill_memory_12col_pg(aimee_pg_stmt_t *stmt, memory_t *m)
{
   memset(m, 0, sizeof(*m));
   m->id = aimee_pg_column_int64(stmt, 0);
   const char *tier = aimee_pg_column_text(stmt, 1);
   const char *kind = aimee_pg_column_text(stmt, 2);
   const char *k = aimee_pg_column_text(stmt, 3);
   const char *content = aimee_pg_column_text(stmt, 4);
   snprintf(m->tier, sizeof(m->tier), "%s", tier ? tier : "");
   snprintf(m->kind, sizeof(m->kind), "%s", kind ? kind : "");
   snprintf(m->key, sizeof(m->key), "%s", k ? k : "");
   snprintf(m->content, sizeof(m->content), "%s", content ? content : "");
   m->confidence = aimee_pg_column_double(stmt, 5);
   m->use_count = aimee_pg_column_int(stmt, 6);
   const char *lua = aimee_pg_column_text(stmt, 7);
   const char *cat = aimee_pg_column_text(stmt, 8);
   const char *uat = aimee_pg_column_text(stmt, 9);
   const char *src = aimee_pg_column_text(stmt, 10);
   snprintf(m->last_used_at, sizeof(m->last_used_at), "%s", lua ? lua : "");
   snprintf(m->created_at, sizeof(m->created_at), "%s", cat ? cat : "");
   snprintf(m->updated_at, sizeof(m->updated_at), "%s", uat ? uat : "");
   snprintf(m->source_session, sizeof(m->source_session), "%s", src ? src : "");
   m->salience = aimee_pg_column_double(stmt, 11);
   const char *pcat = aimee_pg_column_text(stmt, 12);
   snprintf(m->provenance_category, sizeof(m->provenance_category), "%s",
            pcat ? pcat : "user_stated");
   if (aimee_pg_column_count(stmt) > 13)
   {
      const char *use_cases = aimee_pg_column_text(stmt, 13);
      snprintf(m->use_cases, sizeof(m->use_cases), "%s", use_cases ? use_cases : "");
   }
}
