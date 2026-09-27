/* db2/agent_hints.c: one-shot agent hint lookup + consume — Postgres
 * via libpq. */

#include "agent_hints.h"
#include "db2_internal.h"
#include "db_postgres.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

char *db2_agent_hint_find_and_consume(const char *role, const char *prompt)
{
   if (!role || !prompt)
      return NULL;
   void *conn = db2_conn();
   if (!conn)
      return NULL;

   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn,
                                          "SELECT id, hint FROM agent_hints "
                                          "WHERE role = ?1 AND consumed = 0 AND ?2 LIKE pattern "
                                          "ORDER BY id DESC LIMIT 1",
                                          err, sizeof(err));
   if (!st)
      return NULL;
   aimee_pg_bind_text(st, "?1", role);
   aimee_pg_bind_text(st, "?2", prompt);

   if (aimee_pg_step(st, err, sizeof(err)) != AIMEE_PG_ROW)
   {
      aimee_pg_finalize(st);
      return NULL;
   }

   int64_t id = aimee_pg_column_int64(st, 0);
   const char *hint = aimee_pg_column_text(st, 1);
   char *result = hint ? strdup(hint) : NULL;
   aimee_pg_finalize(st);

   aimee_pg_stmt_t *upd = aimee_pg_prepare(
       conn, "UPDATE agent_hints SET consumed = 1 WHERE id = ?1", err, sizeof(err));
   if (upd)
   {
      aimee_pg_bind_int64(upd, "?1", id);
      (void)aimee_pg_step(upd, err, sizeof(err));
      aimee_pg_finalize(upd);
   }

   return result;
}
