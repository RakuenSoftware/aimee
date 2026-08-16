/* db2/kind_lifecycle.c: per-kind lifecycle thresholds — Postgres via libpq. */

#include "../headers/aimee.h" /* kind_lifecycle_t + PROMOTE/DEMOTE/EXPIRE constants */
#include "kind_lifecycle.h"
#include "db2_internal.h"
#include "db_postgres.h"

#include <stddef.h>

/* Default lifecycle for unknown kinds (matches original fact thresholds).
 * Filled into the caller's struct on every miss so callers can ignore
 * the return value when they only care about getting *some* values. */
static const kind_lifecycle_t default_lifecycle = {
    .promote_use_count = PROMOTE_L1_USE_COUNT,
    .promote_confidence = PROMOTE_L1_CONFIDENCE,
    .demote_days = DEMOTE_L2_DAYS,
    .demote_confidence = DEMOTE_L2_CONFIDENCE,
    .expire_days = EXPIRE_L1_DAYS,
    .demotion_resistance = 1.0,
};

int db2_kind_lifecycle_load(const char *kind, kind_lifecycle_t *out)
{
   if (!out)
      return -1;
   *out = default_lifecycle;
   if (!kind)
      return -1;

   void *conn = db2_conn();
   if (!conn)
      return -1;

   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn,
                                          "SELECT promote_use_count, promote_confidence,"
                                          " demote_days, demote_confidence, expire_days,"
                                          " demotion_resistance"
                                          " FROM kind_lifecycle WHERE kind = ?1",
                                          err, sizeof(err));
   if (!st)
      return -1;

   aimee_pg_bind_text(st, "?1", kind);
   int rc = -1;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      out->promote_use_count = aimee_pg_column_int(st, 0);
      out->promote_confidence = aimee_pg_column_double(st, 1);
      out->demote_days = aimee_pg_column_int(st, 2);
      out->demote_confidence = aimee_pg_column_double(st, 3);
      out->expire_days = aimee_pg_column_int(st, 4);
      out->demotion_resistance = aimee_pg_column_double(st, 5);
      rc = 0;
   }
   aimee_pg_finalize(st);
   return rc;
}
