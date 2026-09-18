/* Remaining native ontology-review lookup. Ingestion and provisional relation
 * creation are owned by the shared Go memory module. */
#include "rel_types_store.h"
#include "../headers/rel_types.h"
#include "db2_internal.h"
#include "db_postgres.h"

#include <stdio.h>
#include <string.h>

#define RTS_ERRBUF 256

int db2_rel_types_resolve(const char *rel_type, long *out_id)
{
   if (!rel_type || !rel_type[0])
      return 0;
   void *conn = db2_conn();
   if (!conn)
      return -1;
   char norm[REL_TYPE_NAME_MAX];
   rel_type_normalize(rel_type, norm, sizeof(norm));
   if (!norm[0])
      return 0;
   static const char *sql = "SELECT id FROM rel_types WHERE rel_type = ?1 LIMIT 1";
   char err[RTS_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", norm);
   int found = 0;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      if (out_id)
         *out_id = (long)aimee_pg_column_int64(st, 0);
      found = 1;
   }
   aimee_pg_finalize(st);
   return found;
}
