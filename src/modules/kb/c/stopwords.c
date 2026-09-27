/* db2/stopwords.c: promoted stopwords — Postgres via libpq. */

#include "stopwords.h"
#include "db2_internal.h"
#include "db_postgres.h"

#include <stddef.h>
#include <stdio.h>

int db2_stopwords_list(char out[][32], int max)
{
   if (!out || max <= 0)
      return 0;
   void *conn = db2_conn();
   if (!conn)
      return 0;

   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, "SELECT word FROM stopwords", err, sizeof(err));
   if (!st)
      return 0;

   int count = 0;
   while (count < max && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      const char *w = aimee_pg_column_text(st, 0);
      if (w)
         snprintf(out[count++], 32, "%s", w);
   }
   aimee_pg_finalize(st);
   return count;
}
