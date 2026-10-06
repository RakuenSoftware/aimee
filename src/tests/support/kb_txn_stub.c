/* kb_txn_stub.c — real (not fake) kb_store_kb_txn_* wrappers for test binaries that
 * link kb objects needing transactions (e.g. kb_curator_index_code_unit.o)
 * without pulling in kb_store/kb_payload.o and its dependency tree. Mirrors the
 * kb_payload.c implementations exactly; under the sqlite shim BEGIN/COMMIT/
 * ROLLBACK behave the same, so fenced-abort paths are exercised for real. */
#include "modules/kb/c/db_postgres.h"
#include "modules/kb/c/kb_store_internal.h"

int kb_store_kb_txn_begin(void)
{
   void *conn = kb_store_conn();
   if (!conn)
      return -1;
   char err[256] = "";
   return aimee_pg_exec(conn, "BEGIN", err, sizeof(err)) == 0 ? 0 : -1;
}

int kb_store_kb_txn_commit(void)
{
   void *conn = kb_store_conn();
   if (!conn)
      return -1;
   char err[256] = "";
   return aimee_pg_exec(conn, "COMMIT", err, sizeof(err)) == 0 ? 0 : -1;
}

void kb_store_kb_txn_rollback(void)
{
   void *conn = kb_store_conn();
   if (!conn)
      return;
   char err[256] = "";
   (void)aimee_pg_exec(conn, "ROLLBACK", err, sizeof(err));
}
