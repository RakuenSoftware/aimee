/* kb_txn_stub.c — real (not fake) db2_kb_txn_* wrappers for test binaries that
 * link kb objects needing transactions (e.g. kb_curator_index_code_unit.o)
 * without pulling in db2/kb_payload.o and its dependency tree. Mirrors the
 * kb_payload.c implementations exactly; under the sqlite shim BEGIN/COMMIT/
 * ROLLBACK behave the same, so fenced-abort paths are exercised for real. */
#include "db2/db_postgres.h"
#include "db2/db2_internal.h"

int db2_kb_txn_begin(void)
{
   void *conn = db2_conn();
   if (!conn)
      return -1;
   char err[256] = "";
   return aimee_pg_exec(conn, "BEGIN", err, sizeof(err)) == 0 ? 0 : -1;
}

int db2_kb_txn_commit(void)
{
   void *conn = db2_conn();
   if (!conn)
      return -1;
   char err[256] = "";
   return aimee_pg_exec(conn, "COMMIT", err, sizeof(err)) == 0 ? 0 : -1;
}

void db2_kb_txn_rollback(void)
{
   void *conn = db2_conn();
   if (!conn)
      return;
   char err[256] = "";
   (void)aimee_pg_exec(conn, "ROLLBACK", err, sizeof(err));
}
