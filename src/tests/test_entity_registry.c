/* Go owns entity identity and mutation. Retain the native rollback consumer's
 * external merge record contract until graph rollback also moves to Go. */
#include "../headers/aimee.h"
#include "../modules/db2/c/fact_mutation.h"
#include "../modules/db2/c/db2_test_shim.h"
#include "../modules/db2/c/db2_internal.h"
#include "../modules/db2/c/db_postgres.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
   db2_test_shim_open();
   char err[256] = "";
   assert(aimee_pg_exec(
              db2_conn(),
              "INSERT INTO entity_registry(canonical_id,kind,status) "
              "VALUES(1001,2,'active'),(1002,2,'active');"
              "BEGIN;INSERT INTO entity_merges(id,from_id,into_id) VALUES(4321,1001,1002);"
              "UPDATE entity_registry SET status='merged',merged_into=1002 WHERE canonical_id=1001",
              err, sizeof(err)) == 0);
   fact_actor_t actor = {.rank = FACT_ACTOR_OPERATOR, .authenticated = 1};
   snprintf(actor.principal, sizeof(actor.principal), "test:operator");
   snprintf(actor.role, sizeof(actor.role), "operator");
   char commit[FACT_COMMIT_ID_MAX], rollback[FACT_COMMIT_ID_MAX];
   assert(db2_fact_graph_record_external_in_txn(&actor, "entity.merge", "entity_merge", "4321",
                                                "merge", "active", "merged", 1, commit) == 0);
   assert(aimee_pg_exec(db2_conn(), "COMMIT", err, sizeof(err)) == 0);
   assert(db2_fact_commit_rollback(&actor, commit, rollback) == 1);
   aimee_pg_stmt_t *st =
       aimee_pg_prepare(db2_conn(),
                        "SELECT r.status,r.merged_into,m.undone FROM entity_registry r JOIN "
                        "entity_merges m ON m.from_id=r.canonical_id WHERE m.id=4321",
                        err, sizeof(err));
   assert(st && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW);
   assert(!strcmp(aimee_pg_column_text(st, 0), "active"));
   assert(aimee_pg_column_int64(st, 1) == 0 && aimee_pg_column_int(st, 2) == 1);
   aimee_pg_finalize(st);
   db2_test_shim_close();
   puts("entity merge rollback contract: passed");
   return 0;
}
