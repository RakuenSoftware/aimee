/* test_embedding_dim.c: embedder-runtime-fetch-autodim §2 — kb_meta records the
 * schema embedding dim on first apply and REFUSES a later mismatch (the dim-drift
 * guard). Runs against the sqlite shim (schema_sqlite.sql provides kb_meta).
 *
 * Note: db2_test_shim_open() already applies the schema, which records the shim's
 * default dim — so the test first clears the row to exercise the fresh-record path
 * deterministically. */
#include "../db2/db_schema.h"
#include "../db2/db2_test_shim.h"
#include "../db2/db2_internal.h"
#include "../db2/db_postgres.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
   printf("embedding-dim: ");
   db2_test_shim_open(); /* applies schema_sqlite.sql, incl. kb_meta */
   void *conn = db2_conn();
   assert(conn);
   char err[256];

   /* Start from a fresh kb_meta (shim apply already recorded a default dim). */
   assert(aimee_pg_exec(conn, "DELETE FROM kb_meta WHERE key = 'schema_embedding_dim'", err,
                        sizeof err) == 0);

   /* First apply at 1024: records schema_embedding_dim, no error. */
   err[0] = '\0';
   assert(db2_embedding_dim_record_or_check(conn, 1024, err, sizeof err) == 0);

   /* Same dim again: matches, no-op, no error. */
   err[0] = '\0';
   assert(db2_embedding_dim_record_or_check(conn, 1024, err, sizeof err) == 0);

   /* A different dim is REFUSED (the columns are sized at the recorded 1024). */
   err[0] = '\0';
   assert(db2_embedding_dim_record_or_check(conn, 2560, err, sizeof err) == -1);
   assert(err[0] != '\0');                            /* a remediation message is set */
   assert(strstr(err, "1024") != NULL);               /* names the recorded dim */
   assert(strstr(err, "2560") != NULL);               /* and the configured dim */
   assert(strstr(err, "retrieval-stack.md") != NULL); /* and points at the remediation doc */

   /* The refusal did not change the recorded dim — 1024 still matches. */
   err[0] = '\0';
   assert(db2_embedding_dim_record_or_check(conn, 1024, err, sizeof err) == 0);

   /* A non-positive dim is rejected up front and must NOT be recorded (else the
    * guard would lock into an unrecoverable "matches forever" state). */
   err[0] = '\0';
   assert(db2_embedding_dim_record_or_check(conn, 0, err, sizeof err) == -1);
   assert(err[0] != '\0');
   err[0] = '\0';
   assert(db2_embedding_dim_record_or_check(conn, -5, err, sizeof err) == -1);
   /* ...and the existing 1024 row is untouched by those rejections. */
   err[0] = '\0';
   assert(db2_embedding_dim_record_or_check(conn, 1024, err, sizeof err) == 0);

   /* A corrupt / non-numeric kb_meta value is refused (not silently treated as
    * "no recorded dim" and re-created) — the operator must repair it. */
   assert(aimee_pg_exec(conn,
                        "UPDATE kb_meta SET value = 'garbage' WHERE key = 'schema_embedding_dim'",
                        err, sizeof err) == 0);
   err[0] = '\0';
   assert(db2_embedding_dim_record_or_check(conn, 1024, err, sizeof err) == -1);
   assert(strstr(err, "corrupt") != NULL || strstr(err, "repair") != NULL);

   /* After a manual repair (clearing the bad row) recording succeeds again. */
   assert(aimee_pg_exec(conn, "DELETE FROM kb_meta WHERE key = 'schema_embedding_dim'", err,
                        sizeof err) == 0);
   err[0] = '\0';
   assert(db2_embedding_dim_record_or_check(conn, 1024, err, sizeof err) == 0);

   db2_test_shim_close();
   printf("ok\n");
   return 0;
}
