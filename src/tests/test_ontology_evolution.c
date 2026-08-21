/* test_ontology_evolution.c: typed-fact §2 self-extending ontology promotion
 * pipeline, against the sqlite shim. P4. */
#include "../headers/aimee.h"
#include "../db2/ontology_evolution.h"
#include "../db2/rel_types_store.h"
#include "../db2/fact_mutation.h"
#include "../db2/db2_test_shim.h"
#include "../db2/db2_internal.h"
#include "../db2/db_postgres.h"
#include "memory_ontology.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void operation_commit(const char *operation, char out[FACT_COMMIT_ID_MAX])
{
   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       db2_conn(), "SELECT commit_id FROM fact_graph_commits WHERE operation=?1 LIMIT 1", err,
       sizeof(err));
   assert(st);
   aimee_pg_bind_text(st, "?1", operation);
   assert(aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW);
   snprintf(out, FACT_COMMIT_ID_MAX, "%s", aimee_pg_column_text(st, 0));
   aimee_pg_finalize(st);
}

int main(void)
{
   db2_test_shim_open();
   assert(db2_rel_types_ensure_seed() == 0);

   /* observe(): first sighting creates the row at 1, repeats bump. Normalized. */
   assert(db2_ontology_eval_observe("frobnicates") == 1);
   assert(db2_ontology_eval_observe("Frobnicates") == 2); /* same after normalize */
   assert(db2_ontology_eval_observe("frobnicates") == 3);
   assert(db2_ontology_eval_count("frobnicates") == 3);
   assert(db2_ontology_eval_count("never seen") == -1);
   assert(db2_ontology_eval_observe("") == -1);

   char st[32];
   assert(db2_ontology_eval_status("frobnicates", st, sizeof(st)) == 1);
   assert(strcmp(st, ONTO_EVAL_PENDING) == 0);
   assert(db2_ontology_eval_status("never seen", st, sizeof(st)) == 0);

   /* A second + third novel type at different counts. */
   assert(db2_ontology_eval_observe("wibbles") == 1);
   assert(db2_ontology_eval_observe("wobbles") == 1);
   assert(db2_ontology_eval_observe("wobbles") == 2);
   assert(db2_ontology_eval_observe("wobbles") == 3);

   /* candidates(threshold): only pending types at/above threshold, most-seen
    * first. At threshold 3: frobnicates (3) and wobbles (3); not wibbles (1). */
   char cand[8][64];
   int n = db2_ontology_eval_candidates(3, cand, 8);
   assert(n == 2);
   /* both present (order is by count desc then name asc; both count 3 -> name). */
   int seen_frob = 0, seen_wob = 0;
   for (int i = 0; i < n; i++)
   {
      if (strcmp(cand[i], "frobnicates") == 0)
         seen_frob = 1;
      if (strcmp(cand[i], "wobbles") == 0)
         seen_wob = 1;
   }
   assert(seen_frob && seen_wob);
   assert(db2_ontology_eval_candidates(0, cand, 8) == -1); /* bad threshold */
   assert(db2_ontology_eval_candidates(99, cand, 8) == 0); /* none that high */
   /* bound LIMIT must cap the result: 3 pending at threshold>=1, capped to 2. */
   assert(db2_ontology_eval_candidates(1, cand, 2) == 2);

   /* approve(): promotes the provisional rel_type to active + marks approved.
    * Stage a provisional row first (as the commit path would). */
   assert(db2_rel_types_stage_provisional("frobnicates") > 0);
   assert(db2_ontology_approve("frobnicates") == 0);
   assert(db2_ontology_eval_status("frobnicates", st, sizeof(st)) == 1);
   assert(strcmp(st, ONTO_EVAL_APPROVED) == 0);
   char approve_commit[FACT_COMMIT_ID_MAX], rollback_commit[FACT_COMMIT_ID_MAX];
   operation_commit("ontology.approve", approve_commit);
   fact_commit_change_t diff[2];
   assert(db2_fact_commit_preview(approve_commit, diff, 2) == 1);
   assert(strcmp(diff[0].object_kind, "relation") == 0 &&
          strcmp(diff[0].object_key, "frobnicates") == 0);
   fact_actor_t operator_actor = {.rank = FACT_ACTOR_OPERATOR, .authenticated = 1};
   snprintf(operator_actor.principal, sizeof(operator_actor.principal), "test:operator");
   snprintf(operator_actor.role, sizeof(operator_actor.role), "operator");
   assert(db2_fact_commit_rollback(&operator_actor, approve_commit, rollback_commit) == 1);
   assert(db2_ontology_eval_status("frobnicates", st, sizeof(st)) == 1);
   assert(strcmp(st, ONTO_EVAL_PENDING) == 0);
   assert(db2_ontology_approve("frobnicates") == 0); /* decision can be applied again */
   /* once approved it is no longer a pending candidate. */
   n = db2_ontology_eval_candidates(3, cand, 8);
   assert(n == 1 && strcmp(cand[0], "wobbles") == 0);
   /* approving an unknown type fails (no evaluation row). */
   assert(db2_ontology_approve("nonexistent_rel") == -1);

   /* map(): wobbles -> works_for (a real seeded type). Records mapped + retires. */
   assert(db2_ontology_map("wobbles", "works_for") == 0);
   assert(db2_ontology_eval_status("wobbles", st, sizeof(st)) == 1);
   assert(strcmp(st, ONTO_EVAL_MAPPED) == 0);
   assert(db2_ontology_map("wibbles", "no_such_canonical") == -1); /* target must exist */
   assert(db2_ontology_map("wibbles", "wibbles") == -1);           /* cannot map to self */
   /* mapped type drops out of pending candidates. */
   assert(db2_ontology_eval_candidates(3, cand, 8) == 0);

   /* reject(): wibbles is rejected and stays rejected even if observed again. */
   assert(db2_ontology_reject("wibbles") == 0);
   assert(db2_ontology_eval_status("wibbles", st, sizeof(st)) == 1);
   assert(strcmp(st, ONTO_EVAL_REJECTED) == 0);
   /* re-observation bumps the count but does NOT reopen a rejected type. */
   assert(db2_ontology_eval_observe("wibbles") == 2);
   assert(db2_ontology_eval_status("wibbles", st, sizeof(st)) == 1);
   assert(strcmp(st, ONTO_EVAL_REJECTED) == 0);
   assert(db2_ontology_eval_candidates(1, cand, 8) == 0); /* nothing pending left */
   /* rejecting an unknown type fails. */
   assert(db2_ontology_reject("nonexistent_rel") == -1);

   db2_test_shim_close();
   printf("ontology_evolution: all tests passed\n");
   return 0;
}
