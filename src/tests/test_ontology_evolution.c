/* Go owns ontology decisions. Keep the native graph rollback consumer's
 * external relation-change contract covered until rollback also moves to Go. */
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
              "INSERT INTO rel_types(rel_type,status,sensitivity) "
              "VALUES('frobnicates','provisional','pii');"
              "INSERT INTO ontology_evaluations(rel_type,occurrence_count,status) "
              "VALUES('frobnicates',3,'pending');BEGIN;"
              "UPDATE rel_types SET status='active' WHERE rel_type='frobnicates';"
              "UPDATE ontology_evaluations SET status='approved' WHERE rel_type='frobnicates'",
              err, sizeof(err)) == 0);
   fact_actor_t actor = {.rank = FACT_ACTOR_OPERATOR, .authenticated = 1};
   snprintf(actor.principal, sizeof(actor.principal), "test:operator");
   snprintf(actor.role, sizeof(actor.role), "operator");
   char commit[FACT_COMMIT_ID_MAX], rollback[FACT_COMMIT_ID_MAX];
   assert(db2_fact_graph_record_external_in_txn(&actor, "ontology.approve", "relation",
                                                "frobnicates", "promote", "provisional/pending",
                                                "active/approved", 1, commit) == 0);
   assert(aimee_pg_exec(db2_conn(), "COMMIT", err, sizeof(err)) == 0);
   fact_commit_change_t diff[2];
   assert(db2_fact_commit_preview(commit, diff, 2) == 1);
   assert(!strcmp(diff[0].object_kind, "relation") && !strcmp(diff[0].object_key, "frobnicates"));
   assert(db2_fact_commit_rollback(&actor, commit, rollback) == 1);
   aimee_pg_stmt_t *st =
       aimee_pg_prepare(db2_conn(),
                        "SELECT r.status,o.status FROM rel_types r JOIN ontology_evaluations o "
                        "ON o.rel_type=r.rel_type WHERE r.rel_type='frobnicates'",
                        err, sizeof(err));
   assert(st && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW);
   assert(!strcmp(aimee_pg_column_text(st, 0), "provisional"));
   assert(!strcmp(aimee_pg_column_text(st, 1), "pending"));
   aimee_pg_finalize(st);
   db2_test_shim_close();
   puts("ontology relation rollback contract: passed");
   return 0;
}
