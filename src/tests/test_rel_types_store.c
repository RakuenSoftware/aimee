/* test_rel_types_store.c: DB2-backed typed-fact store + commit path (typed-fact
 * §1 / P1b), exercised against the in-memory sqlite shim. */
#include "../headers/aimee.h"
#include "../headers/memory.h" /* edge_t (needs aimee.h first) */
#include "../db2/rel_types_store.h"
#include "../db2/entity_edges.h"
#include "../db2/db2_test_shim.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static int semantic_count(const char *entity)
{
   edge_t e[64];
   return db2_entity_edges_semantic_by_entity(entity, e, 64);
}

int main(void)
{
   db2_test_shim_open();
   assert(db2_rel_types_ensure_seed() == 0);
   assert(db2_rel_types_ensure_seed() == 0); /* idempotent */

   /* Resolve: seeded names (normalized), absent names. */
   long id = 0;
   assert(db2_rel_types_resolve("works_for", &id) == 1 && id > 0);
   long id2 = 0;
   assert(db2_rel_types_resolve("Works For", &id2) == 1 && id2 == id); /* normalization */
   assert(db2_rel_types_resolve("definitely_not_a_relation", NULL) == 0);

   /* ACCEPT -> a semantic edge is written. */
   assert(db2_fact_commit("alice", NODE_PERSON, "works_for", "acme", NODE_ORG, FACT_AUTHORITY_MODEL,
                          1) == FACT_GATE_ACCEPT);
   edge_t e[8];
   int n = db2_entity_edges_semantic_by_entity("alice", e, 8);
   assert(n == 1);
   assert(strcmp(e[0].relation, "works_for") == 0);
   assert(strcmp(e[0].source, "alice") == 0 && strcmp(e[0].target, "acme") == 0);

   /* REJECT_KIND -> no write. */
   assert(db2_fact_commit("printer", NODE_DEVICE, "works_for", "acme", NODE_ORG,
                          FACT_AUTHORITY_MODEL, 1) == FACT_GATE_REJECT_KIND);
   assert(semantic_count("printer") == 0);

   /* NOVEL -> staged provisional + a Class-C semantic edge. */
   assert(db2_fact_commit("bob", NODE_PERSON, "frobnicates", "thing", NODE_OTHER,
                          FACT_AUTHORITY_MODEL, 1) == FACT_GATE_NOVEL);
   assert(semantic_count("bob") == 1);
   long pid = 0;
   assert(db2_rel_types_resolve("frobnicates", &pid) == 1 && pid > 0); /* now in table */

   /* Flag off -> verdict still computed, but nothing is written. */
   assert(db2_fact_commit("carol", NODE_PERSON, "works_for", "globex", NODE_ORG,
                          FACT_AUTHORITY_MODEL, 0) == FACT_GATE_ACCEPT);
   assert(semantic_count("carol") == 0);

   /* Re-commit the same triple bumps weight, not a second row. */
   assert(db2_fact_commit("alice", NODE_PERSON, "works_for", "acme", NODE_ORG, FACT_AUTHORITY_MODEL,
                          1) == FACT_GATE_ACCEPT);
   assert(semantic_count("alice") == 1);

   db2_test_shim_close();
   printf("rel_types_store: all tests passed\n");
   return 0;
}
